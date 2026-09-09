#include "../xf_gfx.h"
#include <freerdp/update.h>

static UINT set_surface(RdpgfxClientContext* gfx, UINT16 id, void* surface)
{
	WINPR_UNUSED(id);
	*(void**)gfx->handle = surface;
	return CHANNEL_RC_OK;
}

static void* get_surface(RdpgfxClientContext* gfx, UINT16 id)
{
	WINPR_UNUSED(id);
	return *(void**)gfx->handle;
}

static UINT get_ids(RdpgfxClientContext* gfx, UINT16** ids, UINT16* count)
{
	*count = get_surface(gfx, 0) ? 1 : 0;
	*ids = calloc(*count, sizeof(UINT16));
	return *count && !*ids ? CHANNEL_RC_NO_MEMORY : CHANNEL_RC_OK;
}

static BOOL resize_desktop(rdpContext* context)
{
	WINPR_UNUSED(context);
	return TRUE;
}

static BOOL check_display(const char* name, BOOL expect_shm)
{
	xfContext xfc = WINPR_C_ARRAY_INIT;
	rdpGdi gdi = WINPR_C_ARRAY_INIT;
	rdpUpdate update = WINPR_C_ARRAY_INIT;
	RdpgfxClientContext gfx = WINPR_C_ARRAY_INIT;
	xfGfxSurface* surface = nullptr;
	rdpSettings* settings = freerdp_settings_new(0);
	BOOL initialized = FALSE;
	BOOL rc = FALSE;
	xfc.display = XOpenDisplay(name);
	if (!settings || !xfc.display)
		goto out;
	xfc.UseXThreads = TRUE;
	xfc.log = WLog_Get("com.freerdp.client.x11.test");
	xfc.screen = DefaultScreenOfDisplay(xfc.display);
	xfc.visual = DefaultVisualOfScreen(xfc.screen);
	xfc.depth = DefaultDepthOfScreen(xfc.screen);
	xfc.scanline_pad = 32;
	xfc.common.context.settings = settings;
	xfc.common.context.gdi = &gdi;
	xfc.common.context.update = &update;
	gdi.context = &xfc.common.context;
	gdi.dstFormat = PIXEL_FORMAT_BGRA32;
	update.DesktopResize = resize_desktop;
	gfx.handle = &surface;
	gfx.GetSurfaceData = get_surface;
	gfx.SetSurfaceData = set_surface;
	gfx.GetSurfaceIds = get_ids;
	if (!freerdp_settings_set_bool(settings, FreeRDP_SoftwareGdi, FALSE) ||
	    !freerdp_settings_set_bool(settings, FreeRDP_DeactivateClientDecoding, TRUE))
		goto out;
	xf_graphics_pipeline_init(&xfc, &gfx);
	initialized = TRUE;
	gfx.codecs = freerdp_client_codecs_new(0);
	if (!gfx.codecs)
		goto out;
	const UINT16 sizes[][2] = { { 320, 240 }, { 333, 257 }, { 1921, 1081 }, { 2560, 1440 } };
	for (size_t i = 0; i < ARRAYSIZE(sizes); i++)
	{
		RDPGFX_CREATE_SURFACE_PDU create = { .surfaceId = 0,
			                                 .width = sizes[i][0],
			                                 .height = sizes[i][1],
			                                 .pixelFormat = GFX_PIXEL_FORMAT_XRGB_8888 };
		RDPGFX_DELETE_SURFACE_PDU remove = { .surfaceId = 0 };
		RDPGFX_RESET_GRAPHICS_PDU reset = { .width = sizes[i][0], .height = sizes[i][1] };
		if (gfx.CreateSurface(&gfx, &create) || !surface)
			goto out;
		const BOOL is_shm = surface->image->obdata != nullptr;
		if (is_shm != expect_shm)
		{
			fprintf(stderr, "%s: expected SHM=%d, got %d\n", name, expect_shm, is_shm);
			goto out;
		}
		if (gfx.ResetGraphics(&gfx, &reset) || gfx.DeleteSurface(&gfx, &remove))
			goto out;
	}
	rc = TRUE;
out:
	if (surface && initialized)
	{
		RDPGFX_DELETE_SURFACE_PDU remove = { .surfaceId = 0 };
		if (gfx.DeleteSurface(&gfx, &remove) != CHANNEL_RC_OK)
			rc = FALSE;
	}
	if (initialized)
		xf_graphics_pipeline_uninit(&xfc, &gfx);
	if (xfc.display)
		XCloseDisplay(xfc.display);
	freerdp_settings_free(settings);
	return rc;
}

int main(int argc, char* argv[])
{
	WINPR_UNUSED(argc);
	WINPR_UNUSED(argv);
	XInitThreads();
	const char* no_shm = getenv("NO_SHM_DISPLAY");
	const char* shm = getenv("DISPLAY");
	if (!no_shm || !shm)
		return 1;
	return check_display(no_shm, FALSE) && check_display(shm, TRUE) && check_display(no_shm, FALSE)
	           ? 0
	           : 1;
}
