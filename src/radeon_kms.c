/*
 * Copyright © 2009 Red Hat, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * Authors:
 *    Dave Airlie <airlied@redhat.com>
 *
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <errno.h>
#include <sys/ioctl.h>
/* Driver data structures */
#include "radeon.h"
#include "radeon_reg.h"
#include "radeon_probe.h"
#include "micmap.h"

#include "shadow.h"

#include "atipciids.h"



#ifdef XF86DRM_MODE

#include "radeon_chipset_gen.h"
#include "radeon_chipinfo_gen.h"

#define CURSOR_WIDTH	64
#define CURSOR_HEIGHT	64

#include "radeon_bo_gem.h"
#include "radeon_cs_gem.h"
static Bool radeon_setup_kernel_mem(ScreenPtr pScreen);

const OptionInfoRec RADEONOptions_KMS[] = {
    { OPTION_NOACCEL,        "NoAccel",          OPTV_BOOLEAN, {0}, FALSE },
    { OPTION_SW_CURSOR,      "SWcursor",         OPTV_BOOLEAN, {0}, FALSE },
    { OPTION_PAGE_FLIP,      "EnablePageFlip",   OPTV_BOOLEAN, {0}, FALSE },
    { OPTION_ACCEL_DFS,      "AccelDFS",         OPTV_BOOLEAN, {0}, FALSE },
    { OPTION_IGNORE_EDID,    "IgnoreEDID",       OPTV_BOOLEAN, {0}, FALSE },
    { OPTION_COLOR_TILING,   "ColorTiling",      OPTV_BOOLEAN, {0}, FALSE },
    { OPTION_RENDER_ACCEL,   "RenderAccel",      OPTV_BOOLEAN, {0}, FALSE },
    { OPTION_SUBPIXEL_ORDER, "SubPixelOrder",    OPTV_ANYSTR,  {0}, FALSE },
    { OPTION_ACCELMETHOD,    "AccelMethod",      OPTV_STRING,  {0}, FALSE },
    { OPTION_DRI,            "DRI",       	 OPTV_BOOLEAN, {0}, FALSE },
    { OPTION_TVSTD,          "TVStandard",         OPTV_STRING,  {0}, FALSE },
    { OPTION_EXA_VSYNC,         "EXAVSync",        OPTV_BOOLEAN, {0}, FALSE },
    { -1,                    NULL,               OPTV_NONE,    {0}, FALSE }
};

extern _X_EXPORT int gRADEONEntityIndex;

static int getRADEONEntityIndex(void)
{
    return gRADEONEntityIndex;
}

static void *
radeonShadowWindow(ScreenPtr screen, CARD32 row, CARD32 offset, int mode,
		   CARD32 *size, void *closure)
{
    ScrnInfoPtr pScrn = xf86Screens[screen->myNum];
    RADEONInfoPtr  info   = RADEONPTR(pScrn);
    int stride;

    stride = (pScrn->displayWidth * pScrn->bitsPerPixel) / 8;
    *size = stride;

    return ((uint8_t *)info->front_bo->ptr + row * stride + offset);
}

static Bool RADEONCreateScreenResources_KMS(ScreenPtr pScreen)
{
    ScrnInfoPtr pScrn = xf86Screens[pScreen->myNum];
    RADEONInfoPtr  info   = RADEONPTR(pScrn);
    PixmapPtr pixmap;

    pScreen->CreateScreenResources = info->CreateScreenResources;
    if (!(*pScreen->CreateScreenResources)(pScreen))
	return FALSE;
    pScreen->CreateScreenResources = RADEONCreateScreenResources_KMS;

    if (info->r600_shadow_fb) {
	pixmap = pScreen->GetScreenPixmap(pScreen);

	if (!shadowAdd(pScreen, pixmap, shadowUpdatePackedWeak(),
		       radeonShadowWindow, 0, NULL))
	    return FALSE;
    }

    if (info->dri2.enabled) {
	if (info->front_bo) {
	    PixmapPtr pPix = pScreen->GetScreenPixmap(pScreen);
	    radeon_set_pixmap_bo(pPix, info->front_bo);
	}
    }
    return TRUE;
}

static void RADEONBlockHandler_KMS(int i, pointer blockData,
				   pointer pTimeout, pointer pReadmask)
{
    ScreenPtr      pScreen = screenInfo.screens[i];
    ScrnInfoPtr    pScrn   = xf86Screens[i];
    RADEONInfoPtr  info    = RADEONPTR(pScrn);

    pScreen->BlockHandler = info->BlockHandler;
    (*pScreen->BlockHandler) (i, blockData, pTimeout, pReadmask);
    pScreen->BlockHandler = RADEONBlockHandler_KMS;

    if (info->VideoTimerCallback)
	(*info->VideoTimerCallback)(pScrn, currentTime.milliseconds);

    info->accel_state->engineMode = EXA_ENGINEMODE_UNKNOWN;
    radeon_cs_flush_indirect(pScrn);
}

static Bool RADEONPreInitAccel_KMS(ScrnInfoPtr pScrn)
{
    RADEONInfoPtr  info = RADEONPTR(pScrn);

    if (!(info->accel_state = xcalloc(1, sizeof(struct radeon_accel_state)))) {
	xf86DrvMsg(pScrn->scrnIndex, X_ERROR, "Unable to allocate accel_state rec!\n");
	return FALSE;
    }

    if (info->ChipFamily >= CHIP_FAMILY_R600) {
	xf86DrvMsg(pScrn->scrnIndex, X_INFO,
		   "Using shadowfb for KMS on R600+\n");
	info->r600_shadow_fb = TRUE;
	if (!xf86LoadSubModule(pScrn, "shadow"))
	    info->r600_shadow_fb = FALSE;
	return TRUE;
    }


    if ((info->ChipFamily == CHIP_FAMILY_RS100) ||
	(info->ChipFamily == CHIP_FAMILY_RS200) ||
	(info->ChipFamily == CHIP_FAMILY_RS300) ||
	(info->ChipFamily == CHIP_FAMILY_RS400) ||
	(info->ChipFamily == CHIP_FAMILY_RS480) ||
	(info->ChipFamily == CHIP_FAMILY_RS600) ||
	(info->ChipFamily == CHIP_FAMILY_RS690) ||
	(info->ChipFamily == CHIP_FAMILY_RS740))
	info->accel_state->has_tcl = FALSE;
    else {
	info->accel_state->has_tcl = TRUE;
    }

    info->useEXA = TRUE;

    if (info->useEXA) {
	int errmaj = 0, errmin = 0;
	info->exaReq.majorversion = EXA_VERSION_MAJOR;
	info->exaReq.minorversion = EXA_VERSION_MINOR;
	if (!LoadSubModule(pScrn->module, "exa", NULL, NULL, NULL,
			   &info->exaReq, &errmaj, &errmin)) {
	    LoaderErrorMsg(NULL, "exa", errmaj, errmin);
	    return FALSE;
	}
    }

    return TRUE;
}

static Bool RADEONPreInitChipType_KMS(ScrnInfoPtr pScrn)
{
    RADEONInfoPtr  info   = RADEONPTR(pScrn);
    uint32_t cmd_stat;
    int i;

    info->Chipset = PCI_DEV_DEVICE_ID(info->PciInfo);
    pScrn->chipset = (char *)xf86TokenToString(RADEONChipsets, info->Chipset);
    if (!pScrn->chipset) {
	xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
		   "ChipID 0x%04x is not recognized\n", info->Chipset);
	return FALSE;
    }

    if (info->Chipset < 0) {
	xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
		   "Chipset \"%s\" is not recognized\n", pScrn->chipset);
	return FALSE;
    }
    xf86DrvMsg(pScrn->scrnIndex, X_PROBED,
	       "Chipset: \"%s\" (ChipID = 0x%04x)\n",
	       pScrn->chipset,
	       info->Chipset);

    for (i = 0; i < sizeof(RADEONCards) / sizeof(RADEONCardInfo); i++) {
	if (info->Chipset == RADEONCards[i].pci_device_id) {
	    RADEONCardInfo *card = &RADEONCards[i];
	    info->ChipFamily = card->chip_family;
	    info->IsMobility = card->mobility;
	    info->IsIGP = card->igp;
	    break;
	}
    }

    info->cardType = CARD_PCI;

    PCI_READ_LONG(info->PciInfo, &cmd_stat, PCI_CMD_STAT_REG);
    if (cmd_stat & RADEON_CAP_LIST) {
	uint32_t cap_ptr, cap_id;

	PCI_READ_LONG(info->PciInfo, &cap_ptr, RADEON_CAPABILITIES_PTR_PCI_CONFIG);
	cap_ptr &= RADEON_CAP_PTR_MASK;

	while(cap_ptr != RADEON_CAP_ID_NULL) {
	    PCI_READ_LONG(info->PciInfo, &cap_id, cap_ptr);
	    if ((cap_id & 0xff)== RADEON_CAP_ID_AGP) {
		info->cardType = CARD_AGP;
		break;
	    }
	    if ((cap_id & 0xff)== RADEON_CAP_ID_EXP) {
		info->cardType = CARD_PCIE;
		break;
	    }
	    cap_ptr = (cap_id >> 8) & RADEON_CAP_PTR_MASK;
	}
    }


    xf86DrvMsg(pScrn->scrnIndex, X_INFO, "%s card detected\n",
	       (info->cardType==CARD_PCI) ? "PCI" :
		(info->cardType==CARD_PCIE) ? "PCIE" : "AGP");

    /* treat PCIE IGP cards as PCI */
    if (info->cardType == CARD_PCIE && info->IsIGP)
	info->cardType = CARD_PCI;

    if ((info->ChipFamily >= CHIP_FAMILY_R600) && info->IsIGP)
	info->cardType = CARD_PCIE;

    /* not sure about gart table requirements */
    if ((info->ChipFamily == CHIP_FAMILY_RS600) && info->IsIGP)
	info->cardType = CARD_PCIE;

#ifdef RENDER
    info->RenderAccel = xf86ReturnOptValBool(info->Options, OPTION_RENDER_ACCEL,
					     info->Chipset != PCI_CHIP_RN50_515E &&
					     info->Chipset != PCI_CHIP_RN50_5969);
#endif
    return TRUE;
}

static Bool radeon_alloc_dri(ScrnInfoPtr pScrn)
{
    RADEONInfoPtr  info   = RADEONPTR(pScrn);
    if (!(info->dri = xcalloc(1, sizeof(struct radeon_dri)))) {
	xf86DrvMsg(pScrn->scrnIndex, X_ERROR,"Unable to allocate dri rec!\n");
	return FALSE;
    }

    if (!(info->cp = xcalloc(1, sizeof(struct radeon_cp)))) {
	xf86DrvMsg(pScrn->scrnIndex, X_ERROR,"Unable to allocate cp rec!\n");
	return FALSE;
    }
    return TRUE;
}

Bool RADEONPreInit_KMS(ScrnInfoPtr pScrn, int flags)
{
    RADEONInfoPtr     info;
    RADEONEntPtr pRADEONEnt;
    DevUnion* pPriv;
    int zaphod_mask = 0;
    char *bus_id;
    Gamma  zeros = { 0.0, 0.0, 0.0 };

    xf86DrvMsgVerb(pScrn->scrnIndex, X_INFO, RADEON_LOGLEVEL_DEBUG,
		   "RADEONPreInit_KMS\n");
    if (pScrn->numEntities != 1) return FALSE;
    if (!RADEONGetRec(pScrn)) return FALSE;

    info               = RADEONPTR(pScrn);
    info->MMIO         = NULL;
    info->IsSecondary  = FALSE;
    info->IsPrimary = FALSE;
    info->kms_enabled = TRUE;
    info->pEnt         = xf86GetEntityInfo(pScrn->entityList[pScrn->numEntities - 1]);
    if (info->pEnt->location.type != BUS_PCI) goto fail;

    pPriv = xf86GetEntityPrivate(pScrn->entityList[0],
				 getRADEONEntityIndex());
    pRADEONEnt = pPriv->ptr;

    if(xf86IsEntityShared(pScrn->entityList[0]))
    {
        if(xf86IsPrimInitDone(pScrn->entityList[0]))
        {
            info->IsSecondary = TRUE;
            pRADEONEnt->pSecondaryScrn = pScrn;
        }
        else
        {
	    info->IsPrimary = TRUE;
            xf86SetPrimInitDone(pScrn->entityList[0]);
            pRADEONEnt->pPrimaryScrn = pScrn;
            pRADEONEnt->HasSecondary = FALSE;
        }
    }

    info->PciInfo = xf86GetPciInfoForEntity(info->pEnt->index);
    pScrn->monitor     = pScrn->confScreen->monitor;

    if (!RADEONPreInitVisual(pScrn))
	goto fail;

    xf86CollectOptions(pScrn, NULL);
    if (!(info->Options = xalloc(sizeof(RADEONOptions_KMS))))
	goto fail;

    memcpy(info->Options, RADEONOptions_KMS, sizeof(RADEONOptions_KMS));
    xf86ProcessOptions(pScrn->scrnIndex, pScrn->options, info->Options);

    if (!RADEONPreInitWeight(pScrn))
	goto fail;

    if (!RADEONPreInitChipType_KMS(pScrn))
        goto fail;

    if (!radeon_alloc_dri(pScrn))
	return FALSE;

    zaphod_mask = 0xf;
    if (info->IsPrimary)
	zaphod_mask = 0xd;
    if (info->IsSecondary)
	zaphod_mask = 0x2;

    bus_id = DRICreatePCIBusID(info->PciInfo);
    if (drmmode_pre_init(pScrn, &info->drmmode, bus_id, "radeon", pScrn->bitsPerPixel / 8, zaphod_mask) == FALSE) {
	xfree(bus_id);
	xf86DrvMsg(pScrn->scrnIndex, X_ERROR, "Kernel modesetting setup failed\n");
	goto fail;
    }

    info->dri->drmFD = info->drmmode.fd;
    info->dri2.drm_fd = info->drmmode.fd;
    info->dri2.enabled = FALSE;
    xfree(bus_id);
    info->dri->pKernelDRMVersion = drmGetVersion(info->dri->drmFD);
    if (info->dri->pKernelDRMVersion == NULL) {
	xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
		   "RADEONDRIGetVersion failed to get the DRM version\n");
	goto fail;
    }

    {
	struct drm_radeon_gem_info mminfo;

	if (!drmCommandWriteRead(info->dri->drmFD, DRM_RADEON_GEM_INFO, &mminfo, sizeof(mminfo)))
	{
	    info->vram_size = mminfo.vram_visible;
	    info->gart_size = mminfo.gart_size;
	    xf86DrvMsg(pScrn->scrnIndex, X_INFO,
		       "mem size init: gart size :%llx vram size: s:%llx visible:%llx\n",
		       mminfo.gart_size, mminfo.vram_size, mminfo.vram_visible);
	}
    }

    if (info->ChipFamily < CHIP_FAMILY_R600) {
	info->useEXA = TRUE;
	info->directRenderingEnabled = TRUE;
    }

    RADEONSetPitch(pScrn);

    /* Set display resolution */
    xf86SetDpi(pScrn, 0, 0);

	/* Get ScreenInit function */
    if (!xf86LoadSubModule(pScrn, "fb")) return FALSE;

    if (!xf86SetGamma(pScrn, zeros)) return FALSE;

    if (!xf86ReturnOptValBool(info->Options, OPTION_SW_CURSOR, FALSE)) {
	if (!xf86LoadSubModule(pScrn, "ramdac")) return FALSE;
    }

    if (!RADEONPreInitAccel_KMS(pScrn))              goto fail;

    if (pScrn->modes == NULL) {
      xf86DrvMsg(pScrn->scrnIndex, X_ERROR, "No modes.\n");
      goto fail;
   }

    return TRUE;
 fail:
    RADEONFreeRec(pScrn);
    return FALSE;

}

static Bool RADEONCursorInit_KMS(ScreenPtr pScreen)
{
    return xf86_cursors_init (pScreen, CURSOR_WIDTH, CURSOR_HEIGHT,
			      (HARDWARE_CURSOR_TRUECOLOR_AT_8BPP |
			       HARDWARE_CURSOR_AND_SOURCE_WITH_MASK |
			       HARDWARE_CURSOR_SOURCE_MASK_INTERLEAVE_1 |
			       HARDWARE_CURSOR_ARGB));
}

static Bool RADEONSaveScreen_KMS(ScreenPtr pScreen, int mode)
{
    ScrnInfoPtr  pScrn = xf86Screens[pScreen->myNum];
    Bool         unblank;

    xf86DrvMsgVerb(pScrn->scrnIndex, X_INFO, RADEON_LOGLEVEL_DEBUG,
		   "RADEONSaveScreen(%d)\n", mode);

    unblank = xf86IsUnblank(mode);
    if (unblank) SetTimeSinceLastInputEvent();

    if ((pScrn != NULL) && pScrn->vtSema) {
	if (unblank)
	    RADEONUnblank(pScrn);
	else
	    RADEONBlank(pScrn);
    }
    return TRUE;
}

/* Called at the end of each server generation.  Restore the original
 * text mode, unmap video memory, and unwrap and call the saved
 * CloseScreen function.
 */
static Bool RADEONCloseScreen_KMS(int scrnIndex, ScreenPtr pScreen)
{
    ScrnInfoPtr    pScrn = xf86Screens[scrnIndex];
    RADEONInfoPtr  info  = RADEONPTR(pScrn);

    xf86DrvMsgVerb(pScrn->scrnIndex, X_INFO, RADEON_LOGLEVEL_DEBUG,
		   "RADEONCloseScreen\n");

    if (info->accel_state->exa) {
	exaDriverFini(pScreen);
	xfree(info->accel_state->exa);
	info->accel_state->exa = NULL;
    }

    if (info->cursor) xf86DestroyCursorInfoRec(info->cursor);
    info->cursor = NULL;

    pScrn->vtSema = FALSE;
    xf86ClearPrimInitDone(info->pEnt->index);
    pScreen->BlockHandler = info->BlockHandler;
    pScreen->CloseScreen = info->CloseScreen;
    return (*pScreen->CloseScreen)(scrnIndex, pScreen);
}


void RADEONFreeScreen_KMS(int scrnIndex, int flags)
{
    ScrnInfoPtr  pScrn = xf86Screens[scrnIndex];
    RADEONInfoPtr  info  = RADEONPTR(pScrn);

    xf86DrvMsgVerb(pScrn->scrnIndex, X_INFO, RADEON_LOGLEVEL_DEBUG,
		   "RADEONFreeScreen\n");

    /* when server quits at PreInit, we don't need do this anymore*/
    if (!info) return;

    RADEONFreeRec(pScrn);
}

Bool RADEONScreenInit_KMS(int scrnIndex, ScreenPtr pScreen,
			  int argc, char **argv)
{
    ScrnInfoPtr    pScrn = xf86Screens[pScreen->myNum];
    RADEONInfoPtr  info  = RADEONPTR(pScrn);
    int            subPixelOrder = SubPixelUnknown;
    char*          s;
    void *front_ptr;

    pScrn->fbOffset = 0;

    miClearVisualTypes();
    if (!miSetVisualTypes(pScrn->depth,
			  miGetDefaultVisualMask(pScrn->depth),
			  pScrn->rgbBits,
			  pScrn->defaultVisual)) return FALSE;
    miSetPixmapDepths ();

    info->directRenderingEnabled = radeon_dri2_screen_init(pScreen);

    front_ptr = info->FB;

    info->bufmgr = radeon_bo_manager_gem_ctor(info->dri->drmFD);
    if (!info->bufmgr) {
	xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
		   "failed to initialise GEM buffer manager");
	return FALSE;
    }
    drmmode_set_bufmgr(pScrn, &info->drmmode, info->bufmgr);

    info->csm = radeon_cs_manager_gem_ctor(info->dri->drmFD);
    if (!info->csm) {
	xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
		   "failed to initialise command submission manager");
	return FALSE;
    }

    info->cs = radeon_cs_create(info->csm, RADEON_BUFFER_SIZE/4);
    if (!info->cs) {
	xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
		   "failed to initialise command submission buffer");
	return FALSE;
    }

    radeon_cs_set_limit(info->cs, RADEON_GEM_DOMAIN_GTT, info->gart_size);

    radeon_setup_kernel_mem(pScreen);
    front_ptr = info->front_bo->ptr;

    if (info->r600_shadow_fb) {
	info->fb_shadow = xcalloc(1,
				  pScrn->displayWidth * pScrn->virtualY *
				  ((pScrn->bitsPerPixel + 7) >> 3));
	if (info->fb_shadow == NULL) {
	    xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
                       "Failed to allocate shadow framebuffer\n");
	    info->r600_shadow_fb = FALSE;
	} else {
	    if (!fbScreenInit(pScreen, info->fb_shadow,
			      pScrn->virtualX, pScrn->virtualY,
			      pScrn->xDpi, pScrn->yDpi, pScrn->displayWidth,
			      pScrn->bitsPerPixel))
		return FALSE;
	}
    }

    if (info->r600_shadow_fb == FALSE) {
	/* Init fb layer */
	if (!fbScreenInit(pScreen, front_ptr,
			  pScrn->virtualX, pScrn->virtualY,
			  pScrn->xDpi, pScrn->yDpi, pScrn->displayWidth,
			  pScrn->bitsPerPixel))
	    return FALSE;
    }

    xf86SetBlackWhitePixels(pScreen);

    if (pScrn->bitsPerPixel > 8) {
	VisualPtr  visual;

	visual = pScreen->visuals + pScreen->numVisuals;
	while (--visual >= pScreen->visuals) {
	    if ((visual->class | DynamicClass) == DirectColor) {
		visual->offsetRed   = pScrn->offset.red;
		visual->offsetGreen = pScrn->offset.green;
		visual->offsetBlue  = pScrn->offset.blue;
		visual->redMask     = pScrn->mask.red;
		visual->greenMask   = pScrn->mask.green;
		visual->blueMask    = pScrn->mask.blue;
	    }
	}
    }

    /* Must be after RGB order fixed */
    fbPictureInit (pScreen, 0, 0);

#ifdef RENDER
    if ((s = xf86GetOptValString(info->Options, OPTION_SUBPIXEL_ORDER))) {
	if (strcmp(s, "RGB") == 0) subPixelOrder = SubPixelHorizontalRGB;
	else if (strcmp(s, "BGR") == 0) subPixelOrder = SubPixelHorizontalBGR;
	else if (strcmp(s, "NONE") == 0) subPixelOrder = SubPixelNone;
	PictureSetSubpixelOrder (pScreen, subPixelOrder);
    }
#endif

    pScrn->vtSema = TRUE;
    /* Backing store setup */
    xf86DrvMsgVerb(pScrn->scrnIndex, X_INFO, RADEON_LOGLEVEL_DEBUG,
		   "Initializing backing store\n");
    miInitializeBackingStore(pScreen);
    xf86SetBackingStore(pScreen);


    if (info->directRenderingEnabled) {
	xf86DrvMsg(pScrn->scrnIndex, X_INFO, "Direct rendering enabled\n");
    } else {
	xf86DrvMsg(pScrn->scrnIndex, X_WARNING,
		   "Direct rendering disabled\n");
    }

    if (!xf86ReturnOptValBool(info->Options, OPTION_NOACCEL, FALSE)) {
	xf86DrvMsgVerb(pScrn->scrnIndex, X_INFO, RADEON_LOGLEVEL_DEBUG,
		       "Initializing Acceleration\n");
	if (RADEONAccelInit(pScreen)) {
	    xf86DrvMsg(scrnIndex, X_INFO, "Acceleration enabled\n");
	    info->accelOn = TRUE;
	} else {
	    xf86DrvMsg(scrnIndex, X_ERROR,
		       "Acceleration initialization failed\n");
	    xf86DrvMsg(scrnIndex, X_INFO, "Acceleration disabled\n");
	    info->accelOn = FALSE;
	}
    } else {
	xf86DrvMsg(scrnIndex, X_INFO, "Acceleration disabled\n");
	info->accelOn = FALSE;
    }

    /* Init DPMS */
    xf86DrvMsgVerb(pScrn->scrnIndex, X_INFO, RADEON_LOGLEVEL_DEBUG,
		   "Initializing DPMS\n");
    xf86DPMSInit(pScreen, xf86DPMSSet, 0);

    xf86DrvMsgVerb(pScrn->scrnIndex, X_INFO, RADEON_LOGLEVEL_DEBUG,
		   "Initializing Cursor\n");

    /* Set Silken Mouse */
    xf86SetSilkenMouse(pScreen);

    /* Cursor setup */
    miDCInitialize(pScreen, xf86GetPointerScreenFuncs());

    if (!xf86ReturnOptValBool(info->Options, OPTION_SW_CURSOR, FALSE)) {
	if (RADEONCursorInit_KMS(pScreen)) {
	}
    }

    /* Init Xv */
    xf86DrvMsgVerb(pScrn->scrnIndex, X_INFO, RADEON_LOGLEVEL_DEBUG,
		   "Initializing Xv\n");
    RADEONInitVideo(pScreen);

    if (info->r600_shadow_fb == TRUE) {
        if (!shadowSetup(pScreen)) {
            return FALSE;
        }
    }
    pScrn->pScreen = pScreen;

    if (!drmmode_set_desired_modes(pScrn, &info->drmmode))
	return FALSE;

    /* Provide SaveScreen & wrap BlockHandler and CloseScreen */
    /* Wrap CloseScreen */
    info->CloseScreen    = pScreen->CloseScreen;
    pScreen->CloseScreen = RADEONCloseScreen_KMS;
    pScreen->SaveScreen  = RADEONSaveScreen_KMS;
    info->BlockHandler = pScreen->BlockHandler;
    pScreen->BlockHandler = RADEONBlockHandler_KMS;
    info->CreateScreenResources = pScreen->CreateScreenResources;
    pScreen->CreateScreenResources = RADEONCreateScreenResources_KMS;

   if (!xf86CrtcScreenInit (pScreen))
       return FALSE;

   /* Wrap pointer motion to flip touch screen around */
//    info->PointerMoved = pScrn->PointerMoved;
//    pScrn->PointerMoved = RADEONPointerMoved;

    if (!drmmode_setup_colormap(pScreen, pScrn))
	return FALSE;

   /* Note unused options */
    if (serverGeneration == 1)
	xf86ShowUnusedOptions(pScrn->scrnIndex, pScrn->options);

    xf86DrvMsgVerb(pScrn->scrnIndex, X_INFO, RADEON_LOGLEVEL_DEBUG,
		   "RADEONScreenInit finished\n");

    return TRUE;
}

Bool RADEONEnterVT_KMS(int scrnIndex, int flags)
{
    ScrnInfoPtr    pScrn = xf86Screens[scrnIndex];
    RADEONInfoPtr  info  = RADEONPTR(pScrn);
    int ret;

    xf86DrvMsgVerb(pScrn->scrnIndex, X_INFO, RADEON_LOGLEVEL_DEBUG,
		   "RADEONEnterVT_KMS\n");


    ret = ioctl(info->dri->drmFD, DRM_IOCTL_SET_MASTER, NULL);
    if (ret == -EINVAL)
	ErrorF("Unable to retrieve master\n");

    info->accel_state->XInited3D = FALSE;
    info->accel_state->engineMode = EXA_ENGINEMODE_UNKNOWN;

    pScrn->vtSema = TRUE;

    if (!drmmode_set_desired_modes(pScrn, &info->drmmode))
	return FALSE;

    if (info->adaptor)
	RADEONResetVideo(pScrn);

    return TRUE;
}


void RADEONLeaveVT_KMS(int scrnIndex, int flags)
{
    ScrnInfoPtr    pScrn = xf86Screens[scrnIndex];
    RADEONInfoPtr  info  = RADEONPTR(pScrn);

    xf86DrvMsgVerb(pScrn->scrnIndex, X_INFO, RADEON_LOGLEVEL_DEBUG,
		   "RADEONLeaveVT_KMS\n");

    ioctl(info->dri->drmFD, DRM_IOCTL_DROP_MASTER, NULL);

#ifdef HAVE_FREE_SHADOW
    xf86RotateFreeShadow(pScrn);
#endif

    xf86_hide_cursors (pScrn);
    info->accel_state->XInited3D = FALSE;
    info->accel_state->engineMode = EXA_ENGINEMODE_UNKNOWN;

    xf86DrvMsgVerb(pScrn->scrnIndex, X_INFO, RADEON_LOGLEVEL_DEBUG,
		   "Ok, leaving now...\n");
}


Bool RADEONSwitchMode_KMS(int scrnIndex, DisplayModePtr mode, int flags)
{
    ScrnInfoPtr    pScrn       = xf86Screens[scrnIndex];
    Bool ret;
    ret = xf86SetSingleMode (pScrn, mode, RR_Rotate_0);
    return ret;

}

void RADEONAdjustFrame_KMS(int scrnIndex, int x, int y, int flags)
{
    ScrnInfoPtr    pScrn       = xf86Screens[scrnIndex];
    RADEONInfoPtr  info        = RADEONPTR(pScrn);
    drmmode_adjust_frame(pScrn, &info->drmmode, x, y, flags);
    return;
}

static Bool radeon_setup_kernel_mem(ScreenPtr pScreen)
{
    ScrnInfoPtr pScrn = xf86Screens[pScreen->myNum];
    RADEONInfoPtr info = RADEONPTR(pScrn);
    xf86CrtcConfigPtr   xf86_config = XF86_CRTC_CONFIG_PTR(pScrn);
    int cpp = info->CurrentLayout.pixel_bytes;
    int screen_size;
    int stride = pScrn->displayWidth * cpp;
    int total_size_bytes = 0, remain_size_bytes;
    int pagesize = 4096;

    if (info->accel_state->exa != NULL) {
	xf86DrvMsg(pScreen->myNum, X_ERROR, "Memory map already initialized\n");
	return FALSE;
    }
    info->accel_state->exa = exaDriverAlloc();
    if (info->accel_state->exa == NULL)
	return FALSE;

    screen_size = RADEON_ALIGN(pScrn->virtualY, 16) * stride;
    {
	int cursor_size = 64 * 4 * 64;
	int c;

	cursor_size = RADEON_ALIGN(cursor_size, pagesize);
	for (c = 0; c < xf86_config->num_crtc; c++) {
	    /* cursor objects */
	    info->cursor_bo[c] = radeon_bo_open(info->bufmgr, 0, cursor_size,
					      0, RADEON_GEM_DOMAIN_VRAM, 0);
	    if (!info->cursor_bo[c]) {
		return FALSE;
	    }

	    if (radeon_bo_map(info->cursor_bo[c], 1)) {
	      ErrorF("Failed to map cursor buffer memory\n");
	    }

	    drmmode_set_cursor(pScrn, &info->drmmode, c, info->cursor_bo[c]);
	    total_size_bytes += cursor_size;
	}
    }

    screen_size = RADEON_ALIGN(screen_size, pagesize);
    /* keep area front front buffer - but don't allocate it yet */
    total_size_bytes += screen_size;

    /* work out from the mm size what the exa / tex sizes need to be */
    remain_size_bytes = info->vram_size - total_size_bytes;

    info->dri->textureSize = 0;

    info->front_bo = radeon_bo_open(info->bufmgr, 0, screen_size,
				    0, RADEON_GEM_DOMAIN_VRAM, 0);

    xf86DrvMsg(pScrn->scrnIndex, X_INFO, "Front buffer size: %dK\n", info->front_bo->size/1024);
    xf86DrvMsg(pScrn->scrnIndex, X_INFO, "Remaining VRAM size (used for pixmaps): %dK\n", remain_size_bytes/1024);

    /* set the emit limit at 90% of VRAM */
    remain_size_bytes = (remain_size_bytes / 10) * 9;

    radeon_cs_set_limit(info->cs, RADEON_GEM_DOMAIN_VRAM, remain_size_bytes);
    return TRUE;
}

#endif