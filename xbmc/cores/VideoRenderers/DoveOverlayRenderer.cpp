/*
 *      Copyright (C) 2012 Team XBMC
 *      http://www.solid-run.com
 *
 *  This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This Program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with XBMC; see the file COPYING.  If not, write to
 *  the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.
 *  http://www.gnu.org/copyleft/gpl.html
 *
 * Original Dove Overlay Rendere written by Rabeeh Khoury from Solid-Run <support@solid-run.com>
 *
 */

#include "system.h"
#if (defined HAVE_CONFIG_H) && (!defined WIN32)
#include "config.h"
#endif

#if defined(HAS_MARVELL_DOVE)

#undef COLOR_KEY_BLACK
#define COLOR_KEY_ALPHA
#include "DoveOverlayRenderer.h"
#include "dovefb.h"
#include "utils/log.h"
#include <stdlib.h>
#include <malloc.h>
#include "utils/fastmemcpy.h"
#include "guilib/GraphicContext.h"

#if defined(CLASSNAME)
#undef CLASSNAME
#endif

#define CLASSNAME "CDoveOverlayRenderer"

CDoveOverlayRenderer::CDoveOverlayRenderer()
{
  memset(m_SoftPicture, 0, sizeof(OutputBuffer) * NUM_BUFFERS);

  for(int i = 0; i < NUM_BUFFERS; i++)
  {
    m_yuvBuffers[i].plane[0]  = NULL;
    m_yuvBuffers[i].plane[1]  = NULL;
    m_yuvBuffers[i].plane[2]  = NULL;

    m_SoftPicture[i].pPicture = (IppVmetaPicture *)malloc(sizeof(IppVmetaPicture));
    if(m_SoftPicture[i].pPicture)
      memset(m_SoftPicture[i].pPicture, 0, sizeof(IppVmetaPicture));
  }

  m_DllMiscGen        = new DllLibMiscGen();
  m_DllVMETA          = new DllLibVMETA();

  UnInit();
}

CDoveOverlayRenderer::~CDoveOverlayRenderer()
{
  UnInit();

  for(int i = 0; i < NUM_BUFFERS; i++)
  {
    if(m_SoftPicture[i].pPicture)
      free(m_SoftPicture[i].pPicture);
    m_SoftPicture[i].pPicture = NULL;
  }

  m_DllVMETA->Unload();
  m_DllMiscGen->Unload();

  delete m_DllMiscGen;
  delete m_DllVMETA;
}


void CDoveOverlayRenderer::ManageDisplay()
{
  CRect view;

  view.x1 = (float)g_settings.m_ResInfo[m_resolution].Overscan.left;
  view.y1 = (float)g_settings.m_ResInfo[m_resolution].Overscan.top;
  view.x2 = (float)g_settings.m_ResInfo[m_resolution].Overscan.right;
  view.y2 = (float)g_settings.m_ResInfo[m_resolution].Overscan.bottom;

  m_sourceRect.x1 = (float)g_settings.m_currentVideoSettings.m_CropLeft;
  m_sourceRect.y1 = (float)g_settings.m_currentVideoSettings.m_CropTop;
  m_sourceRect.x2 = (float)m_sourceWidth - g_settings.m_currentVideoSettings.m_CropRight;
  m_sourceRect.y2 = (float)m_sourceHeight - g_settings.m_currentVideoSettings.m_CropBottom;

  CalcNormalDisplayRect(view.x1, view.y1, view.Width(), view.Height(), GetAspectRatio() * g_settings.m_fPixelRatio, g_settings.m_fZoomAmount, g_settings.m_fVerticalShift);

  if (CONF_FLAGS_FORMAT_MASK(m_iFlags) == CONF_FLAGS_FORMAT_UYVY)
  {
    m_overlaySurface.videoMode = DOVEFB_VMODE_YUV422PACKED_SWAPYUorV;
    m_overlaySurface.viewPortInfo.ycPitch = (m_sourceRect.x2 - m_sourceRect.x1) * 2;
    m_overlaySurface.viewPortInfo.uvPitch = 0;
  } 
  else if (CONF_FLAGS_FORMAT_MASK(m_iFlags) == CONF_FLAGS_FORMAT_YV12)
  {
    m_overlaySurface.videoMode = DOVEFB_VMODE_YUV420PLANAR;
    m_overlaySurface.viewPortInfo.ycPitch = m_sourceRect.x2 - m_sourceRect.x1;
    m_overlaySurface.viewPortInfo.uvPitch = (m_sourceRect.x2 - m_sourceRect.x1) / 2;
  } 

  m_overlaySurface.viewPortInfo.srcWidth  = m_sourceRect.x2 - m_sourceRect.x1;
  m_overlaySurface.viewPortInfo.srcHeight = m_sourceRect.y2 - m_sourceRect.y1;
  m_overlaySurface.viewPortInfo.zoomXSize = m_destRect.x2 - m_destRect.x1;
  m_overlaySurface.viewPortInfo.zoomYSize = m_destRect.y2 - m_destRect.y1;

  m_overlaySurface.viewPortOffset.xOffset = m_destRect.x1;
  m_overlaySurface.viewPortOffset.yOffset = m_destRect.y1;

}

bool CDoveOverlayRenderer::Configure(unsigned int width, unsigned int height, unsigned int d_width, unsigned int d_height, 
                                     float fps, unsigned int flags, unsigned int format)
{
  if (CONF_FLAGS_FORMAT_MASK(flags) == CONF_FLAGS_FORMAT_NV12)
  {
    CLog::Log(LOGERROR, "%s::%s - Bad format\n", CLASSNAME, __func__);
    return false;
  }

  if (CONF_FLAGS_FORMAT_MASK(flags) != CONF_FLAGS_FORMAT_UYVY && CONF_FLAGS_FORMAT_MASK(flags) != CONF_FLAGS_FORMAT_YV12)
  {
    CLog::Log(LOGERROR, "%s::%s - Unkown format 0x%x", CLASSNAME, __func__, CONF_FLAGS_FORMAT_MASK(flags));
    return false;
  }

  memset (&m_overlaySurface, 0, sizeof(m_overlaySurface));
  m_overlaySurface.videoBufferAddr.startAddr = 0;
  m_overlaySurface.videoBufferAddr.length = 0;//frameSize;
  m_overlaySurface.videoBufferAddr.inputData = 0;
  m_overlaySurface.videoBufferAddr.frameID = 0;

  m_sourceWidth   = width;
  m_sourceHeight  = height;
  m_iFlags        = flags;

  // Calculate the input frame aspect ratio.
  CalculateFrameAspectRatio(d_width, d_height);
  ChooseBestResolution(fps);
  SetViewMode(g_settings.m_currentVideoSettings.m_ViewMode);
  ManageDisplay();

  CLog::Log(LOGDEBUG, "%s::%s - Setting ycPitch to %d, uvPitch to %d\n", CLASSNAME, __func__,
      m_overlaySurface.viewPortInfo.ycPitch ,m_overlaySurface.viewPortInfo.uvPitch);

  CLog::Log(LOGDEBUG, "m_sourceRect.x1 %f m_sourceRect.x2 %f m_sourceRect.y1 %f m_sourceRect.y2 %f m_sourceFrameRatio %f\n",
      m_sourceRect.x1, m_sourceRect.x2, m_sourceRect.y1, m_sourceRect.y2, m_sourceFrameRatio);
  CLog::Log(LOGDEBUG, "m_destRect.x1 %f m_destRect.x2 %f m_destRect.y1 %f m_destRect.y2 %f\n",
      m_destRect.x1, m_destRect.x2, m_destRect.y1, m_destRect.y2);

  m_enabled = 0;

  // Open the framebuffer
  m_overlayfd = open("/dev/fb1", O_RDWR);
  if (m_overlayfd == -1)
  {
    CLog::Log(LOGERROR, "%s::%s - Failed to open framebuffer", CLASSNAME, __func__);
    return false;
  }

  int srcMode = SHM_NORMAL;

  if (ioctl(m_overlayfd, DOVEFB_IOCTL_SET_SRC_MODE, &srcMode) == -1)
  {
    CLog::Log(LOGERROR, "%s::%s - Failed to enable video overlay\n", CLASSNAME, __func__);
    return false;
  }

  if (ioctl(m_overlayfd, DOVEFB_IOCTL_SET_VIDEO_MODE, &m_overlaySurface.videoMode) == -1)
  {
    CLog::Log(LOGERROR, "%s::%s - Failed to setup video mode\n", CLASSNAME, __func__);
    return false;
  }

  if (ioctl(m_overlayfd, DOVEFB_IOCTL_SET_VIEWPORT_INFO, &m_overlaySurface.viewPortInfo) != 0)
  {
    CLog::Log(LOGERROR, "%s::%s - Failed to setup video port\n", CLASSNAME, __func__);
    return false;
  }

  if (ioctl(m_overlayfd, DOVEFB_IOCTL_SET_VID_OFFSET, &m_overlaySurface.viewPortOffset) != 0)
  {
    CLog::Log(LOGERROR, "%s::%s - Failed to setup video port offset\n", CLASSNAME, __func__);
    return false;
  }

  int interpolation = 3; // bi-linear interpolation

  if (ioctl(m_overlayfd, DOVEFB_IOCTL_SET_INTERPOLATION_MODE, &interpolation) != 0)
  {
    CLog::Log(LOGERROR, "%s::%s - Failed to setup video interpolation mode\n", CLASSNAME, __func__);
    return false;
  }

  struct _sColorKeyNAlpha alpha;

  memset (&alpha, 0, sizeof(alpha));

  alpha.mode = DOVEFB_ENABLE_RGB_COLORKEY_MODE;
  alpha.alphapath = DOVEFB_GRA_PATH_ALPHA;
  alpha.config = 0xff;//c0;
#ifdef COLOR_KEY_ALPHA
  alpha.Y_ColorAlpha = 0x02020200;
  alpha.U_ColorAlpha = 0x05050500;
  alpha.V_ColorAlpha = 0x07070700;
#endif
#ifdef COLOR_KEY_BLACK
  alpha.Y_ColorAlpha = 0x0;
  alpha.U_ColorAlpha = 0x0;
  alpha.V_ColorAlpha = 0x0;
#endif

  if (ioctl(m_overlayfd, DOVEFB_IOCTL_SET_COLORKEYnALPHA, &alpha) == -1)
  {
    CLog::Log(LOGERROR, "%s::%s - Failed to configure alpha\n", CLASSNAME, __func__);
    return false;
  }

  for (unsigned int i = 0; i < NUM_BUFFERS; i++)
  {
    FreeYV12Image(i);
    CreateYV12Image(i, m_sourceWidth, m_sourceHeight);
  }

  m_currentBuffer = 0;
  m_bConfigured   = true;

  CLog::Log(LOGDEBUG, "%s::%s - Proper format, continuing\n", CLASSNAME, __func__);

  return m_bConfigured;
}

unsigned int CDoveOverlayRenderer::PreInit()
{
  if(!m_DllVMETA->Load() || !m_DllMiscGen->Load())
    return false;

  UnInit();

  m_currentBuffer = 0;

  m_resolution = g_guiSettings.m_LookAndFeelResolution;
  if ( m_resolution == RES_WINDOW )
    m_resolution = RES_DESKTOP;

  return true;
}

int CDoveOverlayRenderer::GetImage(YV12Image *image, int source, bool readonly)
{
  if(!image)
    return -1;

  /* take next available buffer */
  if( source == AUTOSOURCE)
    source = NextYV12Image();

  YV12Image &im = m_yuvBuffers[source];

  for(int p = 0; p < MAX_PLANES; p++)
  {
    image->plane[p]  = im.plane[p];
    image->stride[p] = im.stride[p];
  }

  image->width    = im.width;
  image->height   = im.height;
  image->flags    = im.flags;
  image->cshift_x = im.cshift_x;
  image->cshift_y = im.cshift_y;

  return source;
}

void CDoveOverlayRenderer::ReleaseImage(int source, bool preserve)
{
}

void CDoveOverlayRenderer::FlipPage(int source)
{
  if (!m_bConfigured)
    return;

  ManageDisplay();

  IppVmetaPicture *pPicture = m_SoftPicture[m_currentBuffer].pPicture;

  struct shm_private_info info;
  info.method = SHM_VMETA;
  ioctl(m_overlayfd, DOVEFB_IOCTL_SET_SRC_MODE, &info.method);

  m_overlaySurface.videoBufferAddr.frameID = 0;

  if(pPicture && pPicture->nPhyAddr)
  {
    m_overlaySurface.videoBufferAddr.startAddr = (unsigned char *)pPicture->nPhyAddr;
    m_overlaySurface.videoBufferAddr.length    = pPicture->nBufSize;
  } 
  else 
  {
    m_overlaySurface.videoBufferAddr.startAddr = NULL;
    m_overlaySurface.videoBufferAddr.length    = 0;
  }

  //ioctl by Solid-Run not in marvel kernel
  //if(ioctl(m_overlayfd, DOVEFB_IOCTL_NEXT_FRAME_PRESENT, &m_SoftPicture[m_currentBuffer].buf) != 0)

  if(ioctl(m_overlayfd, DOVEFB_IOCTL_FLIP_VID_BUFFER, &m_overlaySurface) != 0) 
    CLog::Log(LOGERROR, "%s::%s - Error flipping\n", CLASSNAME, __func__);

  if (m_enabled == 0) 
  {
    m_enabled = 1;

    if (ioctl(m_overlayfd, DOVEFB_IOCTL_SWITCH_VID_OVLY, &m_enabled) == -1)
      CLog::Log(LOGERROR, "%s::%s - Failed to enable video overlay\n", CLASSNAME, __func__);

  }

  /*
   * Is only needed for DOVEFB_IOCTL_NEXT_FRAME_PRESENT
   *
  if (ioctl(m_overlayfd, DOVEFB_IOCTL_WAIT_VSYNC, 0) != 0)
    CLog::Log(LOGERROR, "%s::%s - Error waiting for vsync\n", CLASSNAME, __func__);
  */

  if( source >= 0 && source < NUM_BUFFERS )
    m_currentBuffer = source;
  else
    m_currentBuffer = NextYV12Image();
}

void CDoveOverlayRenderer::Reset()
{
}

void CDoveOverlayRenderer::Update(bool bPauseDrawing)
{
}

void CDoveOverlayRenderer::AddProcessor(YV12Image *image, DVDVideoPicture *pDvdVideoPicture)
{
  if (!m_bConfigured)
    return;

  DrawSlice(pDvdVideoPicture);
}

void CDoveOverlayRenderer::RenderUpdate(bool clear, DWORD flags, DWORD alpha)
{
  if (!m_bConfigured)
    return;

#ifdef COLOR_KEY_ALPHA
  glEnable(GL_SCISSOR_TEST);
  glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
  glClear(GL_COLOR_BUFFER_BIT);
#endif
}

bool CDoveOverlayRenderer::RenderCapture(CRenderCapture* capture)
{
  CLog::Log(LOGERROR, "%s::%s - Not implemented\n", CLASSNAME, __func__);
  return true;
}


unsigned int CDoveOverlayRenderer::DrawSlice(DVDVideoPicture *pDvdVideoPicture)
{
  Ipp32u nPhyAddr = 0;
  Ipp32u nBufSize = 0;

  if(pDvdVideoPicture->vmeta)
  {
    IppVmetaPicture *pPicture = (IppVmetaPicture *)pDvdVideoPicture->vmeta;
    nPhyAddr = pPicture->nPhyAddr;
    nBufSize = pPicture->nBufSize;
  }

  IppVmetaPicture *pPicture = m_SoftPicture[m_currentBuffer].pPicture;

  m_SoftPicture[m_currentBuffer].buf[0] = NULL;
  m_SoftPicture[m_currentBuffer].buf[1] = NULL;
  m_SoftPicture[m_currentBuffer].buf[2] = NULL;

  if(!pPicture)
    return false;

  if(nPhyAddr)
  {
    // Decoder allocated buffer
    pPicture->nPhyAddr = nPhyAddr;
    pPicture->nBufSize = nBufSize;
    m_SoftPicture[m_currentBuffer].bFree = false;
    m_SoftPicture[m_currentBuffer].buf[0] = (unsigned char *)pPicture->nPhyAddr;
    m_SoftPicture[m_currentBuffer].buf[1] = (unsigned char *)pPicture->nPhyAddr + 
      (pDvdVideoPicture->iLineSize[0] * pDvdVideoPicture->iHeight);
    m_SoftPicture[m_currentBuffer].buf[2] = (unsigned char *)pPicture->nPhyAddr + 
      (pDvdVideoPicture->iLineSize[0] * pDvdVideoPicture->iHeight) +
      (pDvdVideoPicture->iLineSize[1] * pDvdVideoPicture->iHeight / 2);
  } 
  else
  {
    // Software decoding. Allocate buffer for ouput
    if(CONF_FLAGS_FORMAT_MASK(m_iFlags) != CONF_FLAGS_FORMAT_YV12)
      return false;

    unsigned int memSize = (pDvdVideoPicture->iLineSize[0] * pDvdVideoPicture->iHeight) +
      (pDvdVideoPicture->iLineSize[1] * pDvdVideoPicture->iHeight / 2) +
      (pDvdVideoPicture->iLineSize[2] * pDvdVideoPicture->iHeight / 2);

    if(!pPicture->pBuf)
      pPicture->pBuf = (Ipp8u*)m_DllVMETA->vdec_os_api_dma_alloc(memSize, VMETA_DIS_BUF_ALIGN, &(pPicture->nPhyAddr));

    if(!pPicture->pBuf)
    {
      CLog::Log(LOGERROR, "%s::%s - Failed to alloc memory\n", CLASSNAME, __func__);
      return false;
    }

    m_SoftPicture[m_currentBuffer].bFree = true;
    m_SoftPicture[m_currentBuffer].buf[0] = (unsigned char *)pPicture->nPhyAddr;
    m_SoftPicture[m_currentBuffer].buf[1] = (unsigned char *)pPicture->nPhyAddr + 
      (pDvdVideoPicture->iLineSize[0] * pDvdVideoPicture->iHeight);
    m_SoftPicture[m_currentBuffer].buf[2] = (unsigned char *)pPicture->nPhyAddr + 
      (pDvdVideoPicture->iLineSize[0] * pDvdVideoPicture->iHeight) +
      (pDvdVideoPicture->iLineSize[1] * pDvdVideoPicture->iHeight / 2);

    unsigned char *dst = (unsigned char *)pPicture->pBuf;

    memset(dst, 0, memSize);

    /*
    int i;

    unsigned char *src = pDvdVideoPicture->data[0];
    for(i = 0; i < pDvdVideoPicture->iHeight; i++)
    {
      fast_memcpy(dst, src, pDvdVideoPicture->iLineSize[0]);
      src += pDvdVideoPicture->iLineSize[0];
      dst += pDvdVideoPicture->iLineSize[0];
    }

    src = pDvdVideoPicture->data[1];
    for(i = 0; i < pDvdVideoPicture->iHeight / 2; i++)
    {
      fast_memcpy(dst, src, pDvdVideoPicture->iLineSize[1]);
      src += pDvdVideoPicture->iLineSize[1];
      dst += pDvdVideoPicture->iLineSize[1];
    }

    src = pDvdVideoPicture->data[2];
    for(i = 0; i < pDvdVideoPicture->iHeight / 2; i++)
    {
      fast_memcpy(dst, src, pDvdVideoPicture->iLineSize[2]);
      src += pDvdVideoPicture->iLineSize[2];
      dst += pDvdVideoPicture->iLineSize[2];
    }
    */

    fast_memcpy( dst, (unsigned char *)pDvdVideoPicture->data[0], pDvdVideoPicture->iLineSize[0] * pDvdVideoPicture->iHeight );
    dst += pDvdVideoPicture->iLineSize[0] * pDvdVideoPicture->iHeight;
    fast_memcpy( dst, (unsigned char *)pDvdVideoPicture->data[1], pDvdVideoPicture->iLineSize[1] * pDvdVideoPicture->iHeight / 2 );
    dst += pDvdVideoPicture->iLineSize[1] * pDvdVideoPicture->iHeight / 2;
    fast_memcpy( dst, (unsigned char *)pDvdVideoPicture->data[2], pDvdVideoPicture->iLineSize[2] * pDvdVideoPicture->iHeight / 2 );
  }
  return 0;
}

void CDoveOverlayRenderer::UnInit()
{
  CLog::Log(LOGDEBUG, "%s::%s\n", CLASSNAME, __func__);

  memset(m_FreeBufAddr, 0, MAX_QUEUE_NUM * sizeof(unsigned char*));
  if(m_overlayfd)
    ioctl(m_overlayfd, DOVEFB_IOCTL_GET_FREELIST, &m_FreeBufAddr);

  if(m_enabled)
  {
    m_enabled                 = 0;

    if (ioctl(m_overlayfd, DOVEFB_IOCTL_WAIT_VSYNC, 0) != 0)
      CLog::Log(LOGERROR, "%s::%s - Error waiting for vsync\n", CLASSNAME, __func__);

    if(ioctl(m_overlayfd, DOVEFB_IOCTL_SWITCH_VID_OVLY, &m_enabled) == -1)
      CLog::Log(LOGERROR, "%s::%s Failed to disable video overlay\n", CLASSNAME, __func__);
  }

  if (m_overlayfd > 0)
    close(m_overlayfd);

  for(int i = 0; i < NUM_BUFFERS; i++)
  {
    FreeYV12Image(i);
    if(m_SoftPicture[i].bFree)
    {
      if(m_SoftPicture[i].pPicture && m_SoftPicture[i].pPicture->pBuf)
        m_DllVMETA->vdec_os_api_dma_free(m_SoftPicture[i].pPicture->pBuf);
      m_SoftPicture[i].pPicture->pBuf = NULL;
      m_SoftPicture[i].bFree = false;
    }
  }

  m_currentBuffer           = 0;
  m_iFlags                  = 0;
  m_bConfigured             = false;
  m_overlayfd               = -1;
  m_sourceWidth             = 0;
  m_sourceHeight            = 0;

  memset(&m_overlaySurface, 0, sizeof(struct _sOvlySurface));
  memset(&m_overlayPlaneInfo, 0, sizeof(struct _sViewPortInfo));
}

void CDoveOverlayRenderer::CreateThumbnail(CBaseTexture* texture, unsigned int width, unsigned int height)
{
  CLog::Log(LOGDEBUG, "%s::%s Was asked to create thumbnail (width = %d, height = %d\n", 
      CLASSNAME, __func__, width, height);
}

bool CDoveOverlayRenderer::Supports(EDEINTERLACEMODE mode)
{
  return false;
}

bool CDoveOverlayRenderer::Supports(ERENDERFEATURE feature)
{
  return false;
}

bool CDoveOverlayRenderer::SupportsMultiPassRendering()
{
  return false;
}

bool CDoveOverlayRenderer::Supports(EINTERLACEMETHOD method)
{
  return false;
}

bool CDoveOverlayRenderer::Supports(ESCALINGMETHOD method)
{
  if(method == VS_SCALINGMETHOD_NEAREST || method == VS_SCALINGMETHOD_LINEAR)
    return true;

  return false;
}

EINTERLACEMETHOD CDoveOverlayRenderer::AutoInterlaceMethod()
{
  return VS_INTERLACEMETHOD_NONE;
}

unsigned int CDoveOverlayRenderer::NextYV12Image()
{
  return (m_currentBuffer + 1) % NUM_BUFFERS;
}

bool CDoveOverlayRenderer::CreateYV12Image(unsigned int index, unsigned int width, unsigned int height)
{
  YV12Image &im = m_yuvBuffers[index];

  im.width  = width;
  im.height = height;
  im.cshift_x = 1;
  im.cshift_y = 1;

  unsigned paddedWidth = (im.width + 15) & ~15;

  im.stride[0] = paddedWidth;
  im.stride[1] = paddedWidth >> im.cshift_x;
  im.stride[2] = paddedWidth >> im.cshift_x;

  im.planesize[0] = im.stride[0] * im.height;
  im.planesize[1] = im.stride[1] * ( im.height >> im.cshift_y );
  im.planesize[2] = im.stride[2] * ( im.height >> im.cshift_y );

  /*
  for (int i = 0; i < MAX_PLANES; i++)
    im.plane[i] = new BYTE[im.planesize[i]];
  */

  return true;
}

bool CDoveOverlayRenderer::FreeYV12Image(unsigned int index)
{
  YV12Image &im = m_yuvBuffers[index];

  for (int i = 0; i < MAX_PLANES; i++)
  {
    //delete[] im.plane[i];
    im.plane[i] = NULL;
  }

  memset(&im , 0, sizeof(YV12Image));

  return true;
}
#endif
