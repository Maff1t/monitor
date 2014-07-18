#include <stdio.h>
#include <stdint.h>
#include <windows.h>
#include <shlwapi.h>
#include "misc.h"
#include "ntapi.h"

static LONG (WINAPI *pNtQueryInformationProcess)(
    HANDLE ProcessHandle,
    ULONG ProcessInformationClass,
    PVOID ProcessInformation,
    ULONG ProcessInformationLength,
    PULONG ReturnLength
);

static LONG (WINAPI *pNtQueryInformationThread)(
    HANDLE ThreadHandle,
    ULONG ThreadInformationClass,
    PVOID ThreadInformation,
    ULONG ThreadInformationLength,
    PULONG ReturnLength
);

static NTSTATUS (WINAPI *pNtQueryAttributesFile)(
    const OBJECT_ATTRIBUTES *ObjectAttributes,
    PFILE_BASIC_INFORMATION FileInformation
);

static NTSTATUS (WINAPI *pNtQueryVolumeInformationFile)(
    HANDLE FileHandle,
    PIO_STATUS_BLOCK IoStatusBlock,
    PVOID FsInformation,
    ULONG Length,
    FS_INFORMATION_CLASS FsInformationClass
);

static NTSTATUS (WINAPI *pNtQueryInformationFile)(
    HANDLE FileHandle,
    PIO_STATUS_BLOCK IoStatusBlock,
    PVOID FileInformation,
    ULONG Length,
    FILE_INFORMATION_CLASS FileInformationClass
);

void misc_init()
{
    HMODULE mod = GetModuleHandle("ntdll");

    *(FARPROC *) &pNtQueryInformationProcess =
        GetProcAddress(mod, "NtQueryInformationProcess");

    *(FARPROC *) &pNtQueryInformationThread =
        GetProcAddress(mod, "NtQueryInformationThread");

    *(FARPROC *) &pNtQueryAttributesFile =
        GetProcAddress(mod, "NtQueryAttributesFile");

    *(FARPROC *) &pNtQueryVolumeInformationFile =
        GetProcAddress(mod, "NtQueryVolumeInformationFile");

    *(FARPROC *) &pNtQueryInformationFile =
        GetProcAddress(mod, "NtQueryInformationFile");
}

uint32_t pid_from_process_handle(HANDLE process_handle)
{
    PROCESS_BASIC_INFORMATION pbi; ULONG size;

    if(NT_SUCCESS(pNtQueryInformationProcess(process_handle,
            ProcessBasicInformation, &pbi, sizeof(pbi), &size)) &&
            size == sizeof(pbi)) {
        return pbi.UniqueProcessId;
    }
    return 0;
}

uint32_t pid_from_thread_handle(HANDLE thread_handle)
{
    THREAD_BASIC_INFORMATION tbi; ULONG size;

    if(NT_SUCCESS(pNtQueryInformationThread(thread_handle,
            ThreadBasicInformation, &tbi, sizeof(tbi), &size)) &&
            size == sizeof(tbi)) {
        return (uint32_t) tbi.ClientId.UniqueProcess;
    }
    return 0;
}

uint32_t parent_process_id()
{
    return pid_from_process_handle(GetCurrentProcess());
}

BOOL is_directory_objattr(const OBJECT_ATTRIBUTES *obj)
{
    FILE_BASIC_INFORMATION info;

    if(NT_SUCCESS(pNtQueryAttributesFile(obj, &info))) {
        return info.FileAttributes & FILE_ATTRIBUTE_DIRECTORY ? TRUE : FALSE;
    }

    return FALSE;
}

// Hide our module from PEB.
// http://www.openrce.org/blog/view/844/How_to_hide_dll

#define CUT_LIST(item) \
    item.Blink->Flink = item.Flink; \
    item.Flink->Blink = item.Blink

void hide_module_from_peb(HMODULE module_handle)
{
    LDR_MODULE *mod; PEB *peb = (PEB *) readfsdword(0x30);

    for (mod = (LDR_MODULE *) peb->LoaderData->InLoadOrderModuleList.Flink;
         mod->BaseAddress != NULL;
         mod = (LDR_MODULE *) mod->InLoadOrderModuleList.Flink) {

        if(mod->BaseAddress == module_handle) {
            CUT_LIST(mod->InLoadOrderModuleList);
            CUT_LIST(mod->InInitializationOrderModuleList);
            CUT_LIST(mod->InMemoryOrderModuleList);
            CUT_LIST(mod->HashTableEntry);

            memset(mod, 0, sizeof(LDR_MODULE));
            break;
        }
    }
}

uint32_t path_from_handle(HANDLE handle,
    wchar_t *path, uint32_t path_buffer_len)
{
    IO_STATUS_BLOCK status; FILE_FS_VOLUME_INFORMATION volume_information;

    unsigned char buf[FILE_NAME_INFORMATION_REQUIRED_SIZE];
    FILE_NAME_INFORMATION *name_information = (FILE_NAME_INFORMATION *) buf;

    // Get the volume serial number of the directory handle.
    if(NT_SUCCESS(pNtQueryVolumeInformationFile(handle, &status,
            &volume_information, sizeof(volume_information),
            FileFsVolumeInformation)) == 0) {
        return 0;
    }

    unsigned long serial_number;

    // Enumerate all harddisks in order to find the
    // corresponding serial number.
    wcscpy(path, L"?:\\");
    for (path[0] = 'A'; path[0] <= 'Z'; path[0]++) {
        if(GetVolumeInformationW(path, NULL, 0, &serial_number, NULL,
                NULL, NULL, 0) == 0 ||
                serial_number != volume_information.VolumeSerialNumber) {
            continue;
        }

        // Obtain the relative path for this filename on the given harddisk.
        if(NT_SUCCESS(pNtQueryInformationFile(handle, &status,
                name_information, FILE_NAME_INFORMATION_REQUIRED_SIZE,
                FileNameInformation)) == 0) {
            continue;
        }

        uint32_t length = name_information->FileNameLength / sizeof(wchar_t);

        // NtQueryInformationFile omits the "C:" part in a filename.
        wcsncpy(path + 2, name_information->FileName, path_buffer_len - 2);

        return length + 2 < path_buffer_len ?
            length + 2 : path_buffer_len - 1;
    }
    return 0;
}

uint32_t path_from_object_attributes(const OBJECT_ATTRIBUTES *obj,
    wchar_t *path, uint32_t buffer_length)
{
    if(obj == NULL || obj->ObjectName == NULL ||
            obj->ObjectName->Buffer == NULL) {
        return 0;
    }

    uint32_t obj_length = obj->ObjectName->Length / sizeof(wchar_t);

    if(obj->RootDirectory == NULL) {
        wcsncpy(path, obj->ObjectName->Buffer, buffer_length);
        return obj_length > buffer_length ? buffer_length : obj_length;
    }

    uint32_t length =
        path_from_handle(obj->RootDirectory, path, buffer_length);

    path[length++] = L'\\';
    wcsncpy(&path[length], obj->ObjectName->Buffer, buffer_length - length);

    length += obj_length;
    return length > buffer_length ? buffer_length : length;
}

int ensure_absolute_path(wchar_t *out, const wchar_t *in, int length)
{
    if(!wcsncmp(in, L"\\??\\", 4)) {
        length -= 4, in += 4;
        wcsncpy(out, in, length < MAX_PATH ? length : MAX_PATH);
        return length;
    }
    else if(in[1] != ':' || (in[2] != '\\' && in[2] != '/')) {
        wchar_t cur_dir[MAX_PATH], fname[MAX_PATH];
        GetCurrentDirectoryW(ARRAYSIZE(cur_dir), cur_dir);

        // Ensure the filename is zero-terminated.
        wcsncpy(fname, in, length < MAX_PATH ? length : MAX_PATH);
        fname[length] = 0;

        PathCombineW(out, cur_dir, fname);
        return lstrlenW(out);
    }
    else {
        wcsncpy(out, in, length < MAX_PATH ? length : MAX_PATH);
        return length;
    }
}
