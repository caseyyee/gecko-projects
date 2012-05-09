/* ***** BEGIN LICENSE BLOCK *****
 * Version: MPL 1.1/GPL 2.0/LGPL 2.1
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * The Original Code is Gonk.
 *
 * The Initial Developer of the Original Code is
 * the Mozilla Foundation.
 * Portions created by the Initial Developer are Copyright (C) 2011
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
 *   Michael Wu <mwu@mozilla.com>
 *
 * Alternatively, the contents of this file may be used under the terms of
 * either the GNU General Public License Version 2 or later (the "GPL"), or
 * the GNU Lesser General Public License Version 2.1 or later (the "LGPL"),
 * in which case the provisions of the GPL or the LGPL are applicable instead
 * of those above. If you wish to allow use of your version of this file only
 * under the terms of either the GPL or the LGPL, and not to allow others to
 * use your version of this file under the terms of the MPL, indicate your
 * decision by deleting the provisions above and replace them with the notice
 * and other provisions required by the GPL or the LGPL. If you do not delete
 * the provisions above, a recipient may use your version of this file under
 * the terms of any one of the MPL, the GPL or the LGPL.
 *
 * ***** END LICENSE BLOCK ***** */

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <fcntl.h>

#include "android/log.h"
#include "ui/FramebufferNativeWindow.h"

#include "mozilla/Hal.h"
#include "mozilla/Preferences.h"
#include "mozilla/FileUtils.h"
#include "Framebuffer.h"
#include "gfxContext.h"
#include "gfxUtils.h"
#include "GLContextProvider.h"
#include "LayerManagerOGL.h"
#include "nsAutoPtr.h"
#include "nsAppShell.h"
#include "nsIdleService.h"
#include "nsScreenManagerGonk.h"
#include "nsTArray.h"
#include "nsWindow.h"

#define LOG(args...)  __android_log_print(ANDROID_LOG_INFO, "Gonk" , ## args)

#define IS_TOPLEVEL() (mWindowType == eWindowType_toplevel || mWindowType == eWindowType_dialog)

using namespace mozilla;
using namespace mozilla::dom;
using namespace mozilla::hal;
using namespace mozilla::gl;
using namespace mozilla::layers;
using namespace mozilla::widget;

nsIntRect gScreenBounds;
static uint32_t sScreenRotation;
static nsIntRect sVirtualBounds;
static gfxMatrix sRotationMatrix;

static nsRefPtr<GLContext> sGLContext;
static nsTArray<nsWindow *> sTopWindows;
static nsWindow *gWindowToRedraw = nsnull;
static nsWindow *gFocusedWindow = nsnull;
static android::FramebufferNativeWindow *gNativeWindow = nsnull;
static bool sFramebufferOpen;
static bool sUsingOMTC;
static nsRefPtr<gfxASurface> sOMTCSurface;
static pthread_t sFramebufferWatchThread;

namespace {

class ScreenOnOffEvent : public nsRunnable {
public:
    ScreenOnOffEvent(bool on)
        : mIsOn(on)
    {}

    NS_IMETHOD Run() {
        nsSizeModeEvent event(true, NS_SIZEMODE, NULL);
        nsEventStatus status;

        event.time = PR_Now() / 1000;
        event.mSizeMode = mIsOn ? nsSizeMode_Fullscreen : nsSizeMode_Minimized;

        for (PRUint32 i = 0; i < sTopWindows.Length(); i++) {
            nsWindow *win = sTopWindows[i];
            event.widget = win;
            win->DispatchEvent(&event, status);
        }

        return NS_OK;
    }

private:
    bool mIsOn;
};

static const char* kSleepFile = "/sys/power/wait_for_fb_sleep";
static const char* kWakeFile = "/sys/power/wait_for_fb_wake";

static void *frameBufferWatcher(void *) {

    int len = 0;
    char buf;

    nsRefPtr<ScreenOnOffEvent> mScreenOnEvent = new ScreenOnOffEvent(true);
    nsRefPtr<ScreenOnOffEvent> mScreenOffEvent = new ScreenOnOffEvent(false);

    while (true) {
        // Cannot use epoll here because kSleepFile and kWakeFile are
        // always ready to read and blocking.
        {
            ScopedClose fd(open(kSleepFile, O_RDONLY, 0));
            do {
                len = read(fd.get(), &buf, 1);
            } while (len < 0 && errno == EINTR);
            NS_WARN_IF_FALSE(len >= 0, "WAIT_FOR_FB_SLEEP failed");
            NS_DispatchToMainThread(mScreenOffEvent);
        }

        {
            ScopedClose fd(open(kWakeFile, O_RDONLY, 0));
            do {
                len = read(fd.get(), &buf, 1);
            } while (len < 0 && errno == EINTR);
            NS_WARN_IF_FALSE(len >= 0, "WAIT_FOR_FB_WAKE failed");
            NS_DispatchToMainThread(mScreenOnEvent);
        }
    }
    
    return NULL;
}

} // anonymous namespace

nsWindow::nsWindow()
{
    if (!sGLContext && !sFramebufferOpen && !sUsingOMTC) {
        // workaround Bug 725143
        hal::SetScreenEnabled(true);

        // Watching screen on/off state by using a pthread
        // which implicitly calls exit() when the main thread ends
        if (pthread_create(&sFramebufferWatchThread, NULL, frameBufferWatcher, NULL)) {
            NS_RUNTIMEABORT("Failed to create framebufferWatcherThread, aborting...");
        }

        sUsingOMTC = Preferences::GetBool("layers.offmainthreadcomposition.enabled", false);

        // We (apparently) don't have a way to tell if allocating the
        // fbs succeeded or failed.
        gNativeWindow = new android::FramebufferNativeWindow();
        if (sUsingOMTC) {
            nsIntSize screenSize;
            bool gotFB = Framebuffer::GetSize(&screenSize);
            MOZ_ASSERT(gotFB);
            gScreenBounds = nsIntRect(nsIntPoint(0, 0), screenSize);

            sOMTCSurface = new gfxImageSurface(gfxIntSize(1, 1),
                gfxASurface::ImageFormatRGB24);
        } else {
            sGLContext = GLContextProvider::CreateForWindow(this);
            // CreateForWindow sets up gScreenBounds
            if (!sGLContext) {
                LOG("Failed to create GL context for fb, trying /dev/graphics/fb0");

                // We can't delete gNativeWindow.

                nsIntSize screenSize;
                sFramebufferOpen = Framebuffer::Open(&screenSize);
                gScreenBounds = nsIntRect(nsIntPoint(0, 0), screenSize);
                if (!sFramebufferOpen) {
                    LOG("Failed to mmap fb(?!?), aborting ...");
                    NS_RUNTIMEABORT("Can't open GL context and can't fall back on /dev/graphics/fb0 ...");
                }
            }
        }
        sVirtualBounds = gScreenBounds;

        nsAppShell::NotifyScreenInitialized();
    }
}

nsWindow::~nsWindow()
{
}

void
nsWindow::DoDraw(void)
{
    if (!hal::GetScreenEnabled()) {
        gDrawRequest = true;
        return;
    }

    if (!gWindowToRedraw) {
        LOG("  no window to draw, bailing");
        return;
    }

    nsPaintEvent event(true, NS_PAINT, gWindowToRedraw);
    event.region = gWindowToRedraw->mDirtyRegion;
    gWindowToRedraw->mDirtyRegion.SetEmpty();

    LayerManager* lm = gWindowToRedraw->GetLayerManager();
    if (LayerManager::LAYERS_OPENGL == lm->GetBackendType()) {
        LayerManagerOGL* oglm = static_cast<LayerManagerOGL*>(lm);
        oglm->SetClippingRegion(event.region);
        oglm->SetWorldTransform(sRotationMatrix);
        gWindowToRedraw->mEventCallback(&event);
    } else if (LayerManager::LAYERS_BASIC == lm->GetBackendType()) {
        MOZ_ASSERT(sFramebufferOpen || sUsingOMTC);
        nsRefPtr<gfxASurface> targetSurface;

        if(sUsingOMTC)
            targetSurface = sOMTCSurface;
        else
            targetSurface = Framebuffer::BackBuffer();

        {
            nsRefPtr<gfxContext> ctx = new gfxContext(targetSurface);
            gfxUtils::PathFromRegion(ctx, event.region);
            ctx->Clip();

            // No double-buffering needed.
            AutoLayerManagerSetup setupLayerManager(
                gWindowToRedraw, ctx, BasicLayerManager::BUFFER_NONE);
            gWindowToRedraw->mEventCallback(&event);
        }

        if (!sUsingOMTC) {
            targetSurface->Flush();
            Framebuffer::Present(event.region);
        }
    } else {
        NS_RUNTIMEABORT("Unexpected layer manager type");
    }
}

nsEventStatus
nsWindow::DispatchInputEvent(nsGUIEvent &aEvent)
{
    if (!gFocusedWindow)
        return nsEventStatus_eIgnore;

    gFocusedWindow->UserActivity();
    aEvent.widget = gFocusedWindow;
    return gFocusedWindow->mEventCallback(&aEvent);
}

NS_IMETHODIMP
nsWindow::Create(nsIWidget *aParent,
                 void *aNativeParent,
                 const nsIntRect &aRect,
                 EVENT_CALLBACK aHandleEventFunction,
                 nsDeviceContext *aContext,
                 nsWidgetInitData *aInitData)
{
    BaseCreate(aParent, IS_TOPLEVEL() ? sVirtualBounds : aRect,
               aHandleEventFunction, aContext, aInitData);

    mBounds = aRect;

    nsWindow *parent = (nsWindow *)aNativeParent;
    mParent = parent;

    if (!aNativeParent) {
        mBounds = sVirtualBounds;
    }

    if (!IS_TOPLEVEL())
        return NS_OK;

    sTopWindows.AppendElement(this);

    Resize(0, 0, sVirtualBounds.width, sVirtualBounds.height, false);
    return NS_OK;
}

NS_IMETHODIMP
nsWindow::Destroy(void)
{
    sTopWindows.RemoveElement(this);
    if (this == gWindowToRedraw)
        gWindowToRedraw = nsnull;
    if (this == gFocusedWindow)
        gFocusedWindow = nsnull;
    return NS_OK;
}

NS_IMETHODIMP
nsWindow::Show(bool aState)
{
    if (mWindowType == eWindowType_invisible)
        return NS_OK;

    if (mVisible == aState)
        return NS_OK;

    mVisible = aState;
    if (!IS_TOPLEVEL())
        return mParent ? mParent->Show(aState) : NS_OK;

    if (aState) {
        BringToTop();
    } else {
        for (unsigned int i = 0; i < sTopWindows.Length(); i++) {
            nsWindow *win = sTopWindows[i];
            if (!win->mVisible)
                continue;

            win->BringToTop();
            break;
        }
    }

    return NS_OK;
}

NS_IMETHODIMP
nsWindow::IsVisible(bool & aState)
{
    aState = mVisible;
    return NS_OK;
}

NS_IMETHODIMP
nsWindow::ConstrainPosition(bool aAllowSlop,
                            PRInt32 *aX,
                            PRInt32 *aY)
{
    return NS_OK;
}

NS_IMETHODIMP
nsWindow::Move(PRInt32 aX,
               PRInt32 aY)
{
    return NS_OK;
}

NS_IMETHODIMP
nsWindow::Resize(PRInt32 aWidth,
                 PRInt32 aHeight,
                 bool    aRepaint)
{
    return Resize(0, 0, aWidth, aHeight, aRepaint);
}

NS_IMETHODIMP
nsWindow::Resize(PRInt32 aX,
                 PRInt32 aY,
                 PRInt32 aWidth,
                 PRInt32 aHeight,
                 bool    aRepaint)
{
    nsSizeEvent event(true, NS_SIZE, this);
    event.time = PR_Now() / 1000;

    nsIntRect rect(aX, aY, aWidth, aHeight);
    mBounds = rect;
    event.windowSize = &rect;
    event.mWinWidth = sVirtualBounds.width;
    event.mWinHeight = sVirtualBounds.height;

    (*mEventCallback)(&event);

    if (aRepaint && gWindowToRedraw)
        gWindowToRedraw->Invalidate(sVirtualBounds);

    return NS_OK;
}

NS_IMETHODIMP
nsWindow::Enable(bool aState)
{
    return NS_OK;
}

NS_IMETHODIMP
nsWindow::IsEnabled(bool *aState)
{
    *aState = true;
    return NS_OK;
}

NS_IMETHODIMP
nsWindow::SetFocus(bool aRaise)
{
    if (aRaise)
        BringToTop();

    gFocusedWindow = this;
    return NS_OK;
}

NS_IMETHODIMP
nsWindow::ConfigureChildren(const nsTArray<nsIWidget::Configuration>&)
{
    return NS_OK;
}

NS_IMETHODIMP
nsWindow::Invalidate(const nsIntRect &aRect)
{
    nsWindow *parent = mParent;
    while (parent && parent != sTopWindows[0])
        parent = parent->mParent;
    if (parent != sTopWindows[0])
        return NS_OK;

    mDirtyRegion.Or(mDirtyRegion, aRect);
    gWindowToRedraw = this;
    gDrawRequest = true;
    mozilla::NotifyEvent();
    return NS_OK;
}

nsIntPoint
nsWindow::WidgetToScreenOffset()
{
    nsIntPoint p(0, 0);
    nsWindow *w = this;

    while (w && w->mParent) {
        p.x += w->mBounds.x;
        p.y += w->mBounds.y;

        w = w->mParent;
    }

    return p;
}

void*
nsWindow::GetNativeData(PRUint32 aDataType)
{
    switch (aDataType) {
    case NS_NATIVE_WINDOW:
        return gNativeWindow;
    case NS_NATIVE_WIDGET:
        return this;
    }
    return nsnull;
}

NS_IMETHODIMP
nsWindow::DispatchEvent(nsGUIEvent *aEvent, nsEventStatus &aStatus)
{
    aStatus = (*mEventCallback)(aEvent);
    return NS_OK;
}

NS_IMETHODIMP_(void)
nsWindow::SetInputContext(const InputContext& aContext,
                          const InputContextAction& aAction)
{
    mInputContext = aContext;
}

NS_IMETHODIMP_(InputContext)
nsWindow::GetInputContext()
{
    return mInputContext;
}

NS_IMETHODIMP
nsWindow::ReparentNativeWidget(nsIWidget* aNewParent)
{
    return NS_OK;
}

float
nsWindow::GetDPI()
{
    return gNativeWindow->xdpi;
}

LayerManager *
nsWindow::GetLayerManager(PLayersChild* aShadowManager,
                          LayersBackend aBackendHint,
                          LayerManagerPersistence aPersistence,
                          bool* aAllowRetaining)
{
    if (aAllowRetaining)
        *aAllowRetaining = true;
    if (mLayerManager)
        return mLayerManager;

    nsWindow *topWindow = sTopWindows[0];

    if (!topWindow) {
        LOG(" -- no topwindow\n");
        return nsnull;
    }

    if (sUsingOMTC) {
        CreateCompositor();
        if (mLayerManager)
            return mLayerManager;
    }

    if (sGLContext) {
        nsRefPtr<LayerManagerOGL> layerManager = new LayerManagerOGL(this);

        if (layerManager->Initialize(sGLContext))
            mLayerManager = layerManager;
        else
            LOG("Could not create OGL LayerManager");
    } else {
        MOZ_ASSERT(sFramebufferOpen);
        mLayerManager = new BasicShadowLayerManager(this);
    }

    return mLayerManager;
}

gfxASurface *
nsWindow::GetThebesSurface()
{
    /* This is really a dummy surface; this is only used when doing reflow, because
     * we need a RenderingContext to measure text against.
     */

    // XXX this really wants to return already_AddRefed, but this only really gets used
    // on direct assignment to a gfxASurface
    return new gfxImageSurface(gfxIntSize(5,5), gfxImageSurface::ImageFormatRGB24);
}

void
nsWindow::BringToTop()
{
    if (!sTopWindows.IsEmpty()) {
        nsGUIEvent event(true, NS_DEACTIVATE, sTopWindows[0]);
        (*mEventCallback)(&event);
    }

    sTopWindows.RemoveElement(this);
    sTopWindows.InsertElementAt(0, this);

    nsGUIEvent event(true, NS_ACTIVATE, this);
    (*mEventCallback)(&event);
    Invalidate(sVirtualBounds);
}

void
nsWindow::UserActivity()
{
    if (!mIdleService) {
        mIdleService = do_GetService("@mozilla.org/widget/idleservice;1");
    }

    if (mIdleService) {
        mIdleService->ResetIdleTimeOut();
    }
}

PRUint32
nsWindow::GetGLFrameBufferFormat()
{
    if (mLayerManager &&
        mLayerManager->GetBackendType() == LayerManager::LAYERS_OPENGL) {
        // We directly map the hardware fb on Gonk.  The hardware fb
        // has RGB format.
        return LOCAL_GL_RGB;
    }
    return LOCAL_GL_NONE;
}

// nsScreenGonk.cpp

nsScreenGonk::nsScreenGonk(void *nativeScreen)
{
}

nsScreenGonk::~nsScreenGonk()
{
}

NS_IMETHODIMP
nsScreenGonk::GetRect(PRInt32 *outLeft,  PRInt32 *outTop,
                      PRInt32 *outWidth, PRInt32 *outHeight)
{
    *outLeft = sVirtualBounds.x;
    *outTop = sVirtualBounds.y;

    *outWidth = sVirtualBounds.width;
    *outHeight = sVirtualBounds.height;

    return NS_OK;
}

NS_IMETHODIMP
nsScreenGonk::GetAvailRect(PRInt32 *outLeft,  PRInt32 *outTop,
                           PRInt32 *outWidth, PRInt32 *outHeight)
{
    return GetRect(outLeft, outTop, outWidth, outHeight);
}

static uint32_t
ColorDepth()
{
    return gNativeWindow->getDevice()->format == GGL_PIXEL_FORMAT_RGB_565 ? 16 : 24;
}

NS_IMETHODIMP
nsScreenGonk::GetPixelDepth(PRInt32 *aPixelDepth)
{
    // XXX: this should actually return 32 when we're using 24-bit
    // color, because we use RGBX.
    *aPixelDepth = ColorDepth();
    return NS_OK;
}

NS_IMETHODIMP
nsScreenGonk::GetColorDepth(PRInt32 *aColorDepth)
{
    return GetPixelDepth(aColorDepth);
}

NS_IMETHODIMP
nsScreenGonk::GetRotation(PRUint32* aRotation)
{
    *aRotation = sScreenRotation;
    return NS_OK;
}

NS_IMETHODIMP
nsScreenGonk::SetRotation(PRUint32 aRotation)
{
    if (!(ROTATION_0_DEG <= aRotation && aRotation <= ROTATION_270_DEG))
        return NS_ERROR_ILLEGAL_VALUE;

    if (sScreenRotation == aRotation)
        return NS_OK;

    sScreenRotation = aRotation;
    sRotationMatrix.Reset();
    switch (aRotation) {
    case nsIScreen::ROTATION_0_DEG:
        sVirtualBounds = gScreenBounds;
        break;
    case nsIScreen::ROTATION_90_DEG:
        sRotationMatrix.Translate(gfxPoint(gScreenBounds.width, 0));
        sRotationMatrix.Rotate(M_PI / 2);
        sVirtualBounds = nsIntRect(0, 0, gScreenBounds.height,
                                         gScreenBounds.width);
        break;
    case nsIScreen::ROTATION_180_DEG:
        sRotationMatrix.Translate(gfxPoint(gScreenBounds.width,
                                           gScreenBounds.height));
        sRotationMatrix.Rotate(M_PI);
        sVirtualBounds = gScreenBounds;
        break;
    case nsIScreen::ROTATION_270_DEG:
        sRotationMatrix.Translate(gfxPoint(0, gScreenBounds.height));
        sRotationMatrix.Rotate(M_PI * 3 / 2);
        sVirtualBounds = nsIntRect(0, 0, gScreenBounds.height,
                                         gScreenBounds.width);
        break;
    default:
        MOZ_NOT_REACHED("Unknown rotation");
        break;
    }

    for (unsigned int i = 0; i < sTopWindows.Length(); i++)
        sTopWindows[i]->Resize(sVirtualBounds.width,
                               sVirtualBounds.height,
                               !i);

    nsAppShell::NotifyScreenRotation();

    return NS_OK;
}

// NB: This isn't gonk-specific, but gonk is the only widget backend
// that does this calculation itself, currently.
static ScreenOrientation
ComputeOrientation(uint32_t aRotation, const nsIntSize& aScreenSize)
{
    bool naturallyPortrait = (aScreenSize.height > aScreenSize.width);
    switch (aRotation) {
    case nsIScreen::ROTATION_0_DEG:
        return (naturallyPortrait ? eScreenOrientation_PortraitPrimary : 
                eScreenOrientation_LandscapePrimary);
    case nsIScreen::ROTATION_90_DEG:
        // Arbitrarily choosing 90deg to be primary "unnatural"
        // rotation.
        return (naturallyPortrait ? eScreenOrientation_LandscapePrimary : 
                eScreenOrientation_PortraitPrimary);
    case nsIScreen::ROTATION_180_DEG:
        return (naturallyPortrait ? eScreenOrientation_PortraitSecondary : 
                eScreenOrientation_LandscapeSecondary);
    case nsIScreen::ROTATION_270_DEG:
        return (naturallyPortrait ? eScreenOrientation_LandscapeSecondary : 
                eScreenOrientation_PortraitSecondary);
    default:
        MOZ_NOT_REACHED("Gonk screen must always have a known rotation");
        return eScreenOrientation_None;
    }
}

/*static*/ uint32_t
nsScreenGonk::GetRotation()
{
    return sScreenRotation;
}

/*static*/ ScreenConfiguration
nsScreenGonk::GetConfiguration()
{
    ScreenOrientation orientation = ComputeOrientation(sScreenRotation,
                                                       gScreenBounds.Size());
    uint32_t colorDepth = ColorDepth();
    // NB: perpetuating colorDepth == pixelDepth illusion here, for
    // consistency.
    return ScreenConfiguration(sVirtualBounds, orientation,
                               colorDepth, colorDepth);
}

NS_IMPL_ISUPPORTS1(nsScreenManagerGonk, nsIScreenManager)

nsScreenManagerGonk::nsScreenManagerGonk()
{
    mOneScreen = new nsScreenGonk(nsnull);
}

nsScreenManagerGonk::~nsScreenManagerGonk()
{
}

NS_IMETHODIMP
nsScreenManagerGonk::GetPrimaryScreen(nsIScreen **outScreen)
{
    NS_IF_ADDREF(*outScreen = mOneScreen.get());
    return NS_OK;
}

NS_IMETHODIMP
nsScreenManagerGonk::ScreenForRect(PRInt32 inLeft,
                                   PRInt32 inTop,
                                   PRInt32 inWidth,
                                   PRInt32 inHeight,
                                   nsIScreen **outScreen)
{
    return GetPrimaryScreen(outScreen);
}

NS_IMETHODIMP
nsScreenManagerGonk::ScreenForNativeWidget(void *aWidget, nsIScreen **outScreen)
{
    return GetPrimaryScreen(outScreen);
}

NS_IMETHODIMP
nsScreenManagerGonk::GetNumberOfScreens(PRUint32 *aNumberOfScreens)
{
    *aNumberOfScreens = 1;
    return NS_OK;
}
