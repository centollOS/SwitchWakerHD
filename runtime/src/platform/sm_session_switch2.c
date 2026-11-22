#include <switch.h>

extern u32 __nx_applet_type;
extern void __libnx_init_time(void);
extern void __libnx_init_cwd(void);
extern bool __libnx_hosversion_is_set(void);
extern void __nx_win_init(void) __attribute__((weak));
extern void userAppInit(void) __attribute__((weak));

static void replaceSmSession(void)
{
	if (smGetServiceOverride(smEncodeName("sm:")) == INVALID_HANDLE)
		return;

	Handle port;
	if (R_FAILED(svcConnectToNamedPort(&port, "sm:")))
		return;

	Service fresh;
	serviceCreate(&fresh, port);
	u64 pidPlaceholder = 0;
	if (R_FAILED(serviceDispatchIn(&fresh, 0, pidPlaceholder, .in_send_pid = true)))
	{
		serviceClose(&fresh);
		return;
	}

	*smGetServiceSession() = fresh;
}

void __appInit(void)
{
	Result rc = smInitialize();
	if (R_FAILED(rc))
		diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_InitFail_SM));
	replaceSmSession();

	if (!__libnx_hosversion_is_set() && R_SUCCEEDED(setsysInitialize()))
	{
		SetSysFirmwareVersion fw;
		if (R_SUCCEEDED(setsysGetFirmwareVersion(&fw)))
			hosversionSet(MAKEHOSVERSION(fw.major, fw.minor, fw.micro));
		setsysExit();
	}

	rc = appletInitialize();
	if (R_FAILED(rc))
		diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_InitFail_AM));

	if (__nx_applet_type != AppletType_None)
	{
		rc = hidInitialize();
		if (R_FAILED(rc))
			diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_InitFail_HID));
	}

	rc = timeInitialize();
	if (R_FAILED(rc))
		diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_InitFail_Time));
	__libnx_init_time();

	rc = fsInitialize();
	if (R_FAILED(rc))
		diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_InitFail_FS));
	fsdevMountSdmc();
	__libnx_init_cwd();

	if (__nx_win_init)
		__nx_win_init();
	if (userAppInit)
		userAppInit();
}
