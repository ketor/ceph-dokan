
#define _UNICODE

#include <windows.h>
#include <winioctl.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <conio.h>
#include <tchar.h>
#include <process.h>
#include <locale.h>
#include <sddl.h>

BOOL MyConvertSecurityDescriptorToStringSecurityDescriptor(
	PSECURITY_DESCRIPTOR SecurityDescriptor,
	DWORD RequestedStringSDRevision,
	SECURITY_INFORMATION SecurityInformation,
	LPTSTR *StringSecurityDescriptor,
	PULONG StringSecurityDescriptorLen
)
{
	return ConvertSecurityDescriptorToStringSecurityDescriptor(
			SecurityDescriptor,
			SDDL_REVISION_1,
			SecurityInformation,
			StringSecurityDescriptor,
			StringSecurityDescriptorLen);
}

BOOL MyConvertStringSecurityDescriptorToSecurityDescriptor(
	LPCTSTR StringSecurityDescriptor,
	DWORD StringSDRevision,
	PSECURITY_DESCRIPTOR *SecurityDescriptor,
	PULONG SecurityDescriptorSize
)
{
	return ConvertStringSecurityDescriptorToSecurityDescriptor(
			StringSecurityDescriptor,
			SDDL_REVISION_1,
			SecurityDescriptor,
			SecurityDescriptorSize);
}

BOOL WINAPI DllMain(
	HINSTANCE	Instance,
	DWORD		Reason,
	LPVOID		Reserved)
{
	return TRUE;
}
