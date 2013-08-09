/*
** Copyright (c) 2011-2012 The Linux Foundation. All Rights Reserved.
**
** Licensed under the Apache License, Version 2.0 (the "License");
** you may not use this file except in compliance with the License.
** You may obtain a copy of the License at
**
**     http://www.apache.org/licenses/LICENSE-2.0
**
** Unless required by applicable law or agreed to in writing, software
** distributed under the License is distributed on an "AS IS" BASIS,
** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
** See the License for the specific language governing permissions and
** limitations under the License.
*/

/*#error uncomment this for compiler test!*/

#define ALOG_TAG "QCameraHWI_Preview"
#include <utils/Log.h>
#include <utils/threads.h>
#include <fcntl.h>
#include <sys/mman.h>
#include "QCameraHAL.h"
#include "QCameraHWI.h"
#include <genlock.h>
#include <gralloc_priv.h>
#include <linux/msm_mdp.h>
#include <cutils/properties.h>

#ifdef ALOGI
#undef ALOGI
#endif
#define ALOGI ALOGE

#define UNLIKELY(exp) __builtin_expect(!!(exp), 0)

#if 1
#define Debug_camera ALOGE
#else
#define Debug_camera do{}while(0)
#endif
/* QCameraHWI_Preview class implementation goes here*/
/* following code implement the preview mode's image capture & display logic of this class*/

namespace android {

// ---------------------------------------------------------------------------
// Preview Callback
// ---------------------------------------------------------------------------
static void preview_notify_cb(mm_camera_ch_data_buf_t *frame,
                                void *user_data)
{
  QCameraStream_preview *pme = (QCameraStream_preview *)user_data;
  mm_camera_ch_data_buf_t *bufs_used = 0;
  ALOGV("%s: E", __func__);
  /* for peview data, there is no queue, so directly use*/
  if(pme==NULL) {
    ALOGE("%s: X : Incorrect cookie",__func__);
    /*Call buf done*/
    return;
  }

  pme->processPreviewFrame(frame);
  ALOGV("%s: X", __func__);
}

status_t QCameraStream_preview::setPreviewWindow(preview_stream_ops_t* window)
{
    status_t retVal = NO_ERROR;
    ALOGE(" %s: E ", __FUNCTION__);
    if( window == NULL) {
        ALOGW(" Setting NULL preview window ");
        /* TODO: Current preview window will be invalidated.
         * Release all the buffers back */
       // relinquishBuffers();
    }
    Mutex::Autolock lock(mStopCallbackLock);
    mPreviewWindow = window;
    ALOGV(" %s : X ", __FUNCTION__ );
    return retVal;
}

status_t QCameraStream_preview::getBufferFromSurface() {
    int err = 0;
    int numMinUndequeuedBufs = 0;
  int format = 0;
  status_t ret = NO_ERROR;
  int gralloc_usage;

    ALOGI(" %s : E ", __FUNCTION__);

    if( mPreviewWindow == NULL) {
    ALOGE("%s: mPreviewWindow = NULL", __func__);
        return INVALID_OPERATION;
  }
    cam_ctrl_dimension_t dim;

  //mDisplayLock.lock();
    ret = cam_config_get_parm(mCameraId, MM_CAMERA_PARM_DIMENSION,&dim);

	format = mHalCamCtrl->getPreviewFormatInfo().Hal_format;
	if(ret != NO_ERROR) {
        ALOGE("%s: display format %d is not supported", __func__, dim.prev_format);
    goto end;
  }
  numMinUndequeuedBufs = 0;
  if(mPreviewWindow->get_min_undequeued_buffer_count) {
    err = mPreviewWindow->get_min_undequeued_buffer_count(mPreviewWindow, &numMinUndequeuedBufs);
    if (err != 0) {
       ALOGE("get_min_undequeued_buffer_count  failed: %s (%d)",
            strerror(-err), -err);
       ret = UNKNOWN_ERROR;
       goto end;
    }
  }
    mHalCamCtrl->mPreviewMemoryLock.lock();
    mHalCamCtrl->mPreviewMemory.buffer_count = kPreviewBufferCount + numMinUndequeuedBufs;
    if(mHalCamCtrl->isZSLMode()) {
      if(mHalCamCtrl->getZSLQueueDepth() > numMinUndequeuedBufs)
        mHalCamCtrl->mPreviewMemory.buffer_count +=
            mHalCamCtrl->getZSLQueueDepth() - numMinUndequeuedBufs;
    }
    err = mPreviewWindow->set_buffer_count(mPreviewWindow, mHalCamCtrl->mPreviewMemory.buffer_count );
    if (err != 0) {
         ALOGE("set_buffer_count failed: %s (%d)",
                    strerror(-err), -err);
         ret = UNKNOWN_ERROR;
     goto end;
    }
    err = mPreviewWindow->set_buffers_geometry(mPreviewWindow,
                dim.display_width, dim.display_height, format);
    if (err != 0) {
         ALOGE("set_buffers_geometry failed: %s (%d)",
                    strerror(-err), -err);
         ret = UNKNOWN_ERROR;
     goto end;
    }

    ret = cam_config_get_parm(mCameraId, MM_CAMERA_PARM_VFE_OUTPUT_ENABLE, &mVFEOutputs);
    if(ret != MM_CAMERA_OK) {
        ALOGE("get parm MM_CAMERA_PARM_VFE_OUTPUT_ENABLE  failed");
        ret = BAD_VALUE;
        goto end;
    }

    //as software encoder is used to encode 720p, to enhance the performance
    //cashed pmem is used here
    if(mVFEOutputs == 1)
        gralloc_usage = CAMERA_GRALLOC_HEAP_ID | CAMERA_GRALLOC_FALLBACK_HEAP_ID;
    else
        gralloc_usage = CAMERA_GRALLOC_HEAP_ID | CAMERA_GRALLOC_FALLBACK_HEAP_ID |
                    CAMERA_GRALLOC_CACHING_ID;
    err = mPreviewWindow->set_usage(mPreviewWindow, gralloc_usage);
    if(err != 0) {
    /* set_usage error out */
        ALOGE("%s: set_usage rc = %d", __func__, err);
        ret = UNKNOWN_ERROR;
        goto end;
    }
    ret = cam_config_get_parm(mCameraId, MM_CAMERA_PARM_HFR_FRAME_SKIP, &mHFRFrameSkip);
    if(ret != MM_CAMERA_OK) {
        ALOGE("get parm MM_CAMERA_PARM_HFR_FRAME_SKIP  failed");
        ret = BAD_VALUE;
        goto end;
    }
	for (int cnt = 0; cnt < mHalCamCtrl->mPreviewMemory.buffer_count; cnt++) {
		int stride;
		err = mPreviewWindow->dequeue_buffer(mPreviewWindow,
										&mHalCamCtrl->mPreviewMemory.buffer_handle[cnt],
										&mHalCamCtrl->mPreviewMemory.stride[cnt]);
		if(!err) {
          ALOGE("%s: dequeue buf hdl =%p, size =%d, stride=%d", __func__, *mHalCamCtrl->mPreviewMemory.buffer_handle[cnt],
                ((struct private_handle_t *)(*mHalCamCtrl->mPreviewMemory.buffer_handle[cnt]))->size,  mHalCamCtrl->mPreviewMemory.stride[cnt]);
                    err = mPreviewWindow->lock_buffer(this->mPreviewWindow,
                                       mHalCamCtrl->mPreviewMemory.buffer_handle[cnt]);
                    // lock the buffer using genlock
                    ALOGE("%s: camera call genlock_lock, hdl=%p", __FUNCTION__, (*mHalCamCtrl->mPreviewMemory.buffer_handle[cnt]));
                    if (GENLOCK_NO_ERROR != genlock_lock_buffer((native_handle_t *)(*mHalCamCtrl->mPreviewMemory.buffer_handle[cnt]),
                                                      GENLOCK_WRITE_LOCK, GENLOCK_MAX_TIMEOUT)) {
                       ALOGE("%s: genlock_lock_buffer(WRITE) failed", __FUNCTION__);
                       mHalCamCtrl->mPreviewMemory.local_flag[cnt] = BUFFER_UNLOCKED;
	                //mHalCamCtrl->mPreviewMemoryLock.unlock();
                       //return -EINVAL;
                   } else {
                     ALOGE("%s: genlock_lock_buffer hdl =%p", __FUNCTION__, *mHalCamCtrl->mPreviewMemory.buffer_handle[cnt]);
                     mHalCamCtrl->mPreviewMemory.local_flag[cnt] = BUFFER_LOCKED;
                   }
		} else {
          mHalCamCtrl->mPreviewMemory.local_flag[cnt] = BUFFER_NOT_OWNED;
          ALOGE("%s: dequeue_buffer idx = %d err = %d", __func__, cnt, err);
        }

		ALOGE("%s: dequeue buf: %p\n", __func__, mHalCamCtrl->mPreviewMemory.buffer_handle[cnt]);

		if(err != 0) {
            ALOGE("%s: dequeue_buffer failed: %s (%d)", __func__,
                    strerror(-err), -err);
            ret = UNKNOWN_ERROR;
			for(int i = 0; i < cnt; i++) {
                if (BUFFER_LOCKED == mHalCamCtrl->mPreviewMemory.local_flag[i]) {
                      ALOGE("%s: camera call genlock_unlock", __FUNCTION__);
                     if (GENLOCK_FAILURE == genlock_unlock_buffer((native_handle_t *)
                                                  (*(mHalCamCtrl->mPreviewMemory.buffer_handle[i])))) {
                        ALOGE("%s: genlock_unlock_buffer failed: hdl =%p", __FUNCTION__, (*(mHalCamCtrl->mPreviewMemory.buffer_handle[i])) );
                         //mHalCamCtrl->mPreviewMemoryLock.unlock();
                        //return -EINVAL;
                     } else {
                       mHalCamCtrl->mPreviewMemory.local_flag[i] = BUFFER_UNLOCKED;
                     }
                }
                if( mHalCamCtrl->mPreviewMemory.local_flag[i] != BUFFER_NOT_OWNED) {
                  err = mPreviewWindow->cancel_buffer(mPreviewWindow,
                                          mHalCamCtrl->mPreviewMemory.buffer_handle[i]);
                }
                mHalCamCtrl->mPreviewMemory.local_flag[i] = BUFFER_NOT_OWNED;
                ALOGE("%s: cancel_buffer: hdl =%p", __func__,  (*mHalCamCtrl->mPreviewMemory.buffer_handle[i]));
				mHalCamCtrl->mPreviewMemory.buffer_handle[i] = NULL;
			}
            memset(&mHalCamCtrl->mPreviewMemory, 0, sizeof(mHalCamCtrl->mPreviewMemory));
			goto end;
		}

		mHalCamCtrl->mPreviewMemory.private_buffer_handle[cnt] =
		    (struct private_handle_t *)(*mHalCamCtrl->mPreviewMemory.buffer_handle[cnt]);
#ifdef USE_ION
        mHalCamCtrl->mPreviewMemory.main_ion_fd[cnt] = open("/dev/ion", O_RDONLY);
        if (mHalCamCtrl->mPreviewMemory.main_ion_fd[cnt] < 0) {
            ALOGE("%s: failed: could not open ion device\n", __func__);
        } else {
            mHalCamCtrl->mPreviewMemory.ion_info_fd[cnt].fd =
                mHalCamCtrl->mPreviewMemory.private_buffer_handle[cnt]->fd;
            if (ioctl(mHalCamCtrl->mPreviewMemory.main_ion_fd[cnt],
              ION_IOC_IMPORT, &mHalCamCtrl->mPreviewMemory.ion_info_fd[cnt]) < 0)
              ALOGE("ION import failed\n");
        }
#endif
		mHalCamCtrl->mPreviewMemory.camera_memory[cnt] =
		    mHalCamCtrl->mGetMemory(mHalCamCtrl->mPreviewMemory.private_buffer_handle[cnt]->fd,
			mHalCamCtrl->mPreviewMemory.private_buffer_handle[cnt]->size, 1, (void *)this);
		ALOGE("%s: idx = %d, fd = %d, size = %d, offset = %d", __func__,
            cnt, mHalCamCtrl->mPreviewMemory.private_buffer_handle[cnt]->fd,
      mHalCamCtrl->mPreviewMemory.private_buffer_handle[cnt]->size,
      mHalCamCtrl->mPreviewMemory.private_buffer_handle[cnt]->offset);
  }


  memset(&mHalCamCtrl->mMetadata, 0, sizeof(mHalCamCtrl->mMetadata));
  memset(mHalCamCtrl->mFace, 0, sizeof(mHalCamCtrl->mFace));

    ALOGI(" %s : X ",__FUNCTION__);
end:
  //mDisplayLock.unlock();
  mHalCamCtrl->mPreviewMemoryLock.unlock();

    return ret;
}

status_t QCameraStream_preview::putBufferToSurface() {
    int err = 0;
    status_t ret = NO_ERROR;

    ALOGI(" %s : E ", __FUNCTION__);

    mHalCamCtrl->mPreviewMemoryLock.lock();
	for (int cnt = 0; cnt < mHalCamCtrl->mPreviewMemory.buffer_count; cnt++) {
        if (cnt < mHalCamCtrl->mPreviewMemory.buffer_count) {
            if (NO_ERROR != mHalCamCtrl->sendUnMappingBuf(MSM_V4L2_EXT_CAPTURE_MODE_PREVIEW, cnt, mCameraId,
                                                          CAM_SOCK_MSG_TYPE_FD_UNMAPPING)) {
                ALOGE("%s: sending data Msg Failed", __func__);
            }
        }

        mHalCamCtrl->mPreviewMemory.camera_memory[cnt]->release(mHalCamCtrl->mPreviewMemory.camera_memory[cnt]);
#ifdef USE_ION
        struct ion_handle_data ion_handle;
        ion_handle.handle = mHalCamCtrl->mPreviewMemory.ion_info_fd[cnt].handle;
        if (ioctl(mHalCamCtrl->mPreviewMemory.main_ion_fd[cnt], ION_IOC_FREE, &ion_handle)
            < 0)
            ALOGE("%s: ion free failed\n", __func__);
        close(mHalCamCtrl->mPreviewMemory.main_ion_fd[cnt]);
#endif
            if (BUFFER_LOCKED == mHalCamCtrl->mPreviewMemory.local_flag[cnt]) {
                ALOGD("%s: camera call genlock_unlock", __FUNCTION__);
	        if (GENLOCK_FAILURE == genlock_unlock_buffer((native_handle_t *)
                                                    (*(mHalCamCtrl->mPreviewMemory.buffer_handle[cnt])))) {
                    ALOGE("%s: genlock_unlock_buffer failed, handle =%p", __FUNCTION__, (*(mHalCamCtrl->mPreviewMemory.buffer_handle[cnt])));
                    continue;
	                //mHalCamCtrl->mPreviewMemoryLock.unlock();
                    //return -EINVAL;
                } else {

                    ALOGD("%s: genlock_unlock_buffer, handle =%p", __FUNCTION__, (*(mHalCamCtrl->mPreviewMemory.buffer_handle[cnt])));
                    mHalCamCtrl->mPreviewMemory.local_flag[cnt] = BUFFER_UNLOCKED;
                }
            }
             if( mHalCamCtrl->mPreviewMemory.local_flag[cnt] != BUFFER_NOT_OWNED) {
               err = mPreviewWindow->cancel_buffer(mPreviewWindow, mHalCamCtrl->mPreviewMemory.buffer_handle[cnt]);
               ALOGD("%s: cancel_buffer: hdl =%p", __func__,  (*mHalCamCtrl->mPreviewMemory.buffer_handle[cnt]));
             }
             mHalCamCtrl->mPreviewMemory.local_flag[cnt] = BUFFER_NOT_OWNED;

		ALOGD(" put buffer %d successfully", cnt);
	}

    if (mDisplayBuf.preview.buf.mp != NULL) {
        delete[] mDisplayBuf.preview.buf.mp;
        mDisplayBuf.preview.buf.mp = NULL;
    }

    mHalCamCtrl->mPreviewMemoryLock.unlock();
	memset(&mHalCamCtrl->mPreviewMemory, 0, sizeof(mHalCamCtrl->mPreviewMemory));
    ALOGI(" %s : X ",__FUNCTION__);
    return NO_ERROR;
}

status_t  QCameraStream_preview::getBufferNoDisplay( )
{
  int err = 0;
  status_t ret = NO_ERROR;
  int i, frame_len, y_off, cbcr_off;
  uint8_t num_planes;
  cam_ctrl_dimension_t dim;
  uint32_t planes[VIDEO_MAX_PLANES];

  ALOGI("%s : E ", __FUNCTION__);


  ret = cam_config_get_parm(mCameraId, MM_CAMERA_PARM_DIMENSION, &dim);
  if(ret != NO_ERROR) {
      ALOGE("%s: display format %d is not supported", __func__, dim.prev_format);
    goto end;
  }
  mHalCamCtrl->mPreviewMemoryLock.lock();
  mHalCamCtrl->mNoDispPreviewMemory.buffer_count = kPreviewBufferCount;
  if(mHalCamCtrl->isZSLMode()) {
    if(mHalCamCtrl->getZSLQueueDepth() > kPreviewBufferCount - 3)
      mHalCamCtrl->mNoDispPreviewMemory.buffer_count =
      mHalCamCtrl->getZSLQueueDepth() + 3;
  }

  num_planes = dim.display_frame_offset.num_planes;
  for ( i = 0; i < num_planes; i++) {
    planes[i] = dim.display_frame_offset.mp[i].len;
  }
  frame_len = dim.display_frame_offset.frame_len;
  y_off = dim.display_frame_offset.mp[0].offset;
  cbcr_off = dim.display_frame_offset.mp[1].offset;

  ALOGE("%s: preview: rotation = %d, yoff = %d, cbcroff = %d, size = %d, width = %d, height = %d",
       __func__, dim.rotation, y_off, cbcr_off, frame_len,
       dim.display_width, dim.display_height);
  if (mHalCamCtrl->initHeapMem(&mHalCamCtrl->mNoDispPreviewMemory,
     mHalCamCtrl->mNoDispPreviewMemory.buffer_count,
     frame_len, y_off, cbcr_off, MSM_PMEM_MAINIMG,
     NULL,NULL, num_planes, planes) < 0) {
              ret = NO_MEMORY;
              goto end;
  };

  memset(&mHalCamCtrl->mMetadata, 0, sizeof(mHalCamCtrl->mMetadata));
  memset(mHalCamCtrl->mFace, 0, sizeof(mHalCamCtrl->mFace));

  ALOGI(" %s : X ",__FUNCTION__);
end:
  mHalCamCtrl->mPreviewMemoryLock.unlock();

  return NO_ERROR;
}

status_t   QCameraStream_preview::freeBufferNoDisplay()
{
  int err = 0;
  status_t ret = NO_ERROR;

  ALOGI(" %s : E ", __FUNCTION__);

  mHalCamCtrl->mPreviewMemoryLock.lock();
  for (int cnt = 0; cnt < mHalCamCtrl->mNoDispPreviewMemory.buffer_count; cnt++) {
      if (cnt < mHalCamCtrl->mNoDispPreviewMemory.buffer_count) {
          if (NO_ERROR != mHalCamCtrl->sendUnMappingBuf(MSM_V4L2_EXT_CAPTURE_MODE_PREVIEW,
                       cnt, mCameraId, CAM_SOCK_MSG_TYPE_FD_UNMAPPING)) {
              ALOGE("%s: sending data Msg Failed", __func__);
          }
      }
  }
  mHalCamCtrl->releaseHeapMem(&mHalCamCtrl->mNoDispPreviewMemory);
  memset(&mHalCamCtrl->mNoDispPreviewMemory, 0, sizeof(mHalCamCtrl->mNoDispPreviewMemory));
  if (mDisplayBuf.preview.buf.mp != NULL) {
      delete[] mDisplayBuf.preview.buf.mp;
      mDisplayBuf.preview.buf.mp = NULL;
  }

  mHalCamCtrl->mPreviewMemoryLock.unlock();
  ALOGI(" %s : X ",__FUNCTION__);
  return NO_ERROR;
}

void QCameraStream_preview::notifyROIEvent(fd_roi_t roi)
{
    switch (roi.type) {
    case FD_ROI_TYPE_HEADER:
        {
            mDisplayLock.lock();
            mNumFDRcvd = 0;
            memset(mHalCamCtrl->mFace, 0, sizeof(mHalCamCtrl->mFace));
            mHalCamCtrl->mMetadata.faces = mHalCamCtrl->mFace;
            mHalCamCtrl->mMetadata.number_of_faces = roi.d.hdr.num_face_detected;
            if(mHalCamCtrl->mMetadata.number_of_faces > MAX_ROI)
              mHalCamCtrl->mMetadata.number_of_faces = MAX_ROI;
            mDisplayLock.unlock();

            if (mHalCamCtrl->mMetadata.number_of_faces == 0) {
                // Clear previous faces
                mHalCamCtrl->mCallbackLock.lock();
                camera_data_callback pcb = mHalCamCtrl->mDataCb;
                mHalCamCtrl->mCallbackLock.unlock();

                if (pcb && (mHalCamCtrl->mMsgEnabled & CAMERA_MSG_PREVIEW_METADATA)){
                    ALOGE("%s: Face detection RIO callback", __func__);
                    pcb(CAMERA_MSG_PREVIEW_METADATA, NULL, 0, &mHalCamCtrl->mMetadata, mHalCamCtrl->mCallbackCookie);
                }
            }
        }
        break;
    case FD_ROI_TYPE_DATA:
        {
            mDisplayLock.lock();
            int idx = roi.d.data.idx;
            if (idx >= mHalCamCtrl->mMetadata.number_of_faces) {
                mDisplayLock.unlock();
                ALOGE("%s: idx %d out of boundary %d", __func__, idx, mHalCamCtrl->mMetadata.number_of_faces);
                break;
            }

            mHalCamCtrl->mFace[idx].id = roi.d.data.face.id;
            mHalCamCtrl->mFace[idx].score = roi.d.data.face.score;

            // top
            mHalCamCtrl->mFace[idx].rect[0] =
               roi.d.data.face.face_boundary.x*2000/mHalCamCtrl->mDimension.display_width - 1000;
            //right
            mHalCamCtrl->mFace[idx].rect[1] =
               roi.d.data.face.face_boundary.y*2000/mHalCamCtrl->mDimension.display_height - 1000;
            //bottom
            mHalCamCtrl->mFace[idx].rect[2] =  mHalCamCtrl->mFace[idx].rect[0] +
               roi.d.data.face.face_boundary.dx*2000/mHalCamCtrl->mDimension.display_width;
            //left
            mHalCamCtrl->mFace[idx].rect[3] = mHalCamCtrl->mFace[idx].rect[1] +
               roi.d.data.face.face_boundary.dy*2000/mHalCamCtrl->mDimension.display_height;

            // Center of left eye
            mHalCamCtrl->mFace[idx].left_eye[0] =
              roi.d.data.face.left_eye_center[0]*2000/mHalCamCtrl->mDimension.display_width - 1000;
            mHalCamCtrl->mFace[idx].left_eye[1] =
              roi.d.data.face.left_eye_center[1]*2000/mHalCamCtrl->mDimension.display_height - 1000;

            // Center of right eye
            mHalCamCtrl->mFace[idx].right_eye[0] =
              roi.d.data.face.right_eye_center[0]*2000/mHalCamCtrl->mDimension.display_width - 1000;
            mHalCamCtrl->mFace[idx].right_eye[1] =
              roi.d.data.face.right_eye_center[1]*2000/mHalCamCtrl->mDimension.display_height - 1000;

            // Center of mouth
            mHalCamCtrl->mFace[idx].mouth[0] =
              roi.d.data.face.mouth_center[0]*2000/mHalCamCtrl->mDimension.display_width - 1000;
            mHalCamCtrl->mFace[idx].mouth[1] =
              roi.d.data.face.mouth_center[1]*2000/mHalCamCtrl->mDimension.display_height - 1000;

            mHalCamCtrl->mFace[idx].smile_degree = roi.d.data.face.smile_degree;
            mHalCamCtrl->mFace[idx].smile_score = roi.d.data.face.smile_confidence;
            mHalCamCtrl->mFace[idx].blink_detected = roi.d.data.face.blink_detected;
            mHalCamCtrl->mFace[idx].face_recognised = roi.d.data.face.is_face_recognised;
            mHalCamCtrl->mFace[idx].gaze_angle = roi.d.data.face.gaze_angle;

            /* newly added */
            // upscale by 2 to recover from demaen downscaling
            mHalCamCtrl->mFace[idx].updown_dir = roi.d.data.face.updown_dir*2;
            mHalCamCtrl->mFace[idx].leftright_dir = roi.d.data.face.leftright_dir*2;
            mHalCamCtrl->mFace[idx].roll_dir = roi.d.data.face.roll_dir*2;

            mHalCamCtrl->mFace[idx].leye_blink = roi.d.data.face.left_blink;
            mHalCamCtrl->mFace[idx].reye_blink = roi.d.data.face.right_blink;
            mHalCamCtrl->mFace[idx].left_right_gaze = roi.d.data.face.left_right_gaze;
            mHalCamCtrl->mFace[idx].top_bottom_gaze = roi.d.data.face.top_bottom_gaze;
            ALOGE("%s: Face(%d, %d, %d, %d), leftEye(%d, %d), rightEye(%d, %d), mouth(%d, %d), smile(%d, %d), face_recg(%d)", __func__,
               mHalCamCtrl->mFace[idx].rect[0],  mHalCamCtrl->mFace[idx].rect[1],
               mHalCamCtrl->mFace[idx].rect[2],  mHalCamCtrl->mFace[idx].rect[3],
               mHalCamCtrl->mFace[idx].left_eye[0], mHalCamCtrl->mFace[idx].left_eye[1],
               mHalCamCtrl->mFace[idx].right_eye[0], mHalCamCtrl->mFace[idx].right_eye[1],
               mHalCamCtrl->mFace[idx].mouth[0], mHalCamCtrl->mFace[idx].mouth[1],
               mHalCamCtrl->mFace[idx].smile_degree, mHalCamCtrl->mFace[idx].smile_score,
               mHalCamCtrl->mFace[idx].face_recognised);
            ALOGE("%s: gaze(%d, %d, %d), updown(%d), leftright(%d), roll(%d), blink(%d, %d, %d)", __func__,
               mHalCamCtrl->mFace[idx].gaze_angle,  mHalCamCtrl->mFace[idx].left_right_gaze,
               mHalCamCtrl->mFace[idx].top_bottom_gaze,  mHalCamCtrl->mFace[idx].updown_dir,
               mHalCamCtrl->mFace[idx].leftright_dir, mHalCamCtrl->mFace[idx].roll_dir,
               mHalCamCtrl->mFace[idx].blink_detected,
               mHalCamCtrl->mFace[idx].leye_blink, mHalCamCtrl->mFace[idx].reye_blink);

             mNumFDRcvd++;
             mDisplayLock.unlock();

             if (mNumFDRcvd == mHalCamCtrl->mMetadata.number_of_faces) {
                 mHalCamCtrl->mCallbackLock.lock();
                 camera_data_callback pcb = mHalCamCtrl->mDataCb;
                 mHalCamCtrl->mCallbackLock.unlock();

                 if (pcb && (mHalCamCtrl->mMsgEnabled & CAMERA_MSG_PREVIEW_METADATA)){
                     ALOGE("%s: Face detection RIO callback with %d faces detected (score=%d)", __func__, mNumFDRcvd, mHalCamCtrl->mFace[idx].score);
                     pcb(CAMERA_MSG_PREVIEW_METADATA, NULL, 0, &mHalCamCtrl->mMetadata, mHalCamCtrl->mCallbackCookie);
                 }
             }
        }
        break;
    }
}

status_t QCameraStream_preview::initDisplayBuffers()
{
  status_t ret = NO_ERROR;
  int width = 0;  /* width of channel  */
  int height = 0; /* height of channel */
  uint32_t frame_len = 0; /* frame planner length */
  int buffer_num = 4; /* number of buffers for display */
  const char *pmem_region;
  uint8_t num_planes = 0;
  uint32_t planes[VIDEO_MAX_PLANES];
  void *vaddr = NULL;
  cam_ctrl_dimension_t dim;
  int i;

  ALOGE("%s:BEGIN",__func__);
  memset(&mHalCamCtrl->mMetadata, 0, sizeof(camera_frame_metadata_t));
  mHalCamCtrl->mPreviewMemoryLock.lock();
  memset(&mHalCamCtrl->mPreviewMemory, 0, sizeof(mHalCamCtrl->mPreviewMemory));
  mHalCamCtrl->mPreviewMemoryLock.unlock();
  memset(&mNotifyBuffer, 0, sizeof(mNotifyBuffer));

/* get preview size, by qury mm_camera*/
  memset(&dim, 0, sizeof(cam_ctrl_dimension_t));

  memset(&(this->mDisplayStreamBuf),0, sizeof(this->mDisplayStreamBuf));

  ret = cam_config_get_parm(mCameraId, MM_CAMERA_PARM_DIMENSION, &dim);
  if (MM_CAMERA_OK != ret) {
    ALOGE("%s: error - can't get camera dimension!", __func__);
    ALOGE("%s: X", __func__);
    return BAD_VALUE;
  }else {
    width =  dim.display_width,
    height = dim.display_height;
  }

#ifdef USE_ION

  ////Gary
  //char value[10]={0};
  //property_get("hw.vt.camera_enable", value, 0);
  vt_camera_enable = CAMERA_VIDEO_CALL & mHalCamCtrl->mParameters.getInt(QCameraParameters::KEY_QC_CAMERA_MODE);
  //if(strcmp(value,"true")==0) vt_camera_enable = 1;
  //else vt_camera_enable = 0;

  if (vt_camera_enable)
  {
  	ion_rotation_fd = open("/dev/ion", O_RDONLY);
  	if (ion_rotation_fd > 0)
  	{
  		ionAllocData.len = mHalCamCtrl->mPreviewWidth * mHalCamCtrl->mPreviewHeight*3/2;
    	ionAllocData.align = 4096;
 	    ionAllocData.flags = (0x1 << CAMERA_ION_HEAP_ID) | (0x1 << CAMERA_ION_FALLBACK_HEAP_ID);
 	 	ioctl(ion_rotation_fd, ION_IOC_ALLOC,  &ionAllocData);
		ion_fddata.handle = ionAllocData.handle;
		ioctl(ion_rotation_fd, ION_IOC_MAP, &ion_fddata);
  	}
  	else ALOGE("CSVT: ion dev open fail");

  	fb_fd = open("/dev/graphics/fb0", O_RDWR);
  	if (fb_fd <= 0) ALOGE("CSVT: fb dev open fail");
  }

  ////end
#endif

  ret = getBufferFromSurface();
  if(ret != NO_ERROR) {
    ALOGE("%s: cannot get memory from surface texture client, ret = %d", __func__, ret);
    return ret;
  }

  /* set 4 buffers for display */
  mHalCamCtrl->mPreviewMemoryLock.lock();
  memset(&mDisplayStreamBuf, 0, sizeof(mDisplayStreamBuf));
  this->mDisplayStreamBuf.num = mHalCamCtrl->mPreviewMemory.buffer_count;
  this->myMode=myMode; /*Need to assign this in constructor after translating from mask*/
  num_planes = dim.display_frame_offset.num_planes;
  for(i =0; i< num_planes; i++) {
     planes[i] = dim.display_frame_offset.mp[i].len;
  }
   this->mDisplayStreamBuf.frame_len = dim.display_frame_offset.frame_len;

  memset(&mDisplayBuf, 0, sizeof(mDisplayBuf));
  mDisplayBuf.preview.buf.mp = new mm_camera_mp_buf_t[mDisplayStreamBuf.num];
  if (!mDisplayBuf.preview.buf.mp) {
    ALOGE("%s Error allocating memory for mplanar struct ", __func__);
    ret = NO_MEMORY;
    goto error;
  }
  memset(mDisplayBuf.preview.buf.mp, 0,
    mDisplayStreamBuf.num * sizeof(mm_camera_mp_buf_t));

  /*allocate memory for the buffers*/
  for(i = 0; i < mDisplayStreamBuf.num; i++){
	  if (mHalCamCtrl->mPreviewMemory.private_buffer_handle[i] == NULL)
		  continue;
      mDisplayStreamBuf.frame[i].fd = mHalCamCtrl->mPreviewMemory.private_buffer_handle[i]->fd;
      mDisplayStreamBuf.frame[i].cbcr_off = planes[0];
      mDisplayStreamBuf.frame[i].y_off = 0;
      mDisplayStreamBuf.frame[i].path = OUTPUT_TYPE_P;
	  mHalCamCtrl->mPreviewMemory.addr_offset[i] =
	      mHalCamCtrl->mPreviewMemory.private_buffer_handle[i]->offset;
      mDisplayStreamBuf.frame[i].buffer =
          (long unsigned int)mHalCamCtrl->mPreviewMemory.camera_memory[i]->data;
      mDisplayStreamBuf.frame[i].ion_alloc.len = mHalCamCtrl->mPreviewMemory.private_buffer_handle[i]->size;
      mDisplayStreamBuf.frame[i].ion_dev_fd = mHalCamCtrl->mPreviewMemory.main_ion_fd[i];
      mDisplayStreamBuf.frame[i].fd_data = mHalCamCtrl->mPreviewMemory.ion_info_fd[i];

    ALOGE("%s: idx = %d, fd = %d, size = %d, cbcr_offset = %d, y_offset = %d, "
      "offset = %d, vaddr = 0x%x", __func__, i, mDisplayStreamBuf.frame[i].fd,
      mHalCamCtrl->mPreviewMemory.private_buffer_handle[i]->size,
      mDisplayStreamBuf.frame[i].cbcr_off, mDisplayStreamBuf.frame[i].y_off,
      mHalCamCtrl->mPreviewMemory.addr_offset[i],
      (uint32_t)mDisplayStreamBuf.frame[i].buffer);

    ret = mHalCamCtrl->sendMappingBuf(
                        MSM_V4L2_EXT_CAPTURE_MODE_PREVIEW,
                        i,
                        mDisplayStreamBuf.frame[i].fd,
                        mHalCamCtrl->mPreviewMemory.private_buffer_handle[i]->size,
                        mCameraId, CAM_SOCK_MSG_TYPE_FD_MAPPING);
    if (NO_ERROR != ret) {
      ALOGE("%s: sending mapping data Msg Failed", __func__);
      goto error;
    }

    mDisplayBuf.preview.buf.mp[i].frame = mDisplayStreamBuf.frame[i];
    mDisplayBuf.preview.buf.mp[i].frame_offset = mHalCamCtrl->mPreviewMemory.addr_offset[i];
    mDisplayBuf.preview.buf.mp[i].num_planes = num_planes;

    /* Plane 0 needs to be set seperately. Set other planes
     * in a loop. */
    mDisplayBuf.preview.buf.mp[i].planes[0].length = planes[0];
    mDisplayBuf.preview.buf.mp[i].planes[0].m.userptr = mDisplayStreamBuf.frame[i].fd;
    mDisplayBuf.preview.buf.mp[i].planes[0].data_offset = 0;
    mDisplayBuf.preview.buf.mp[i].planes[0].reserved[0] =
      mDisplayBuf.preview.buf.mp[i].frame_offset;
    for (int j = 1; j < num_planes; j++) {
      mDisplayBuf.preview.buf.mp[i].planes[j].length = planes[j];
      mDisplayBuf.preview.buf.mp[i].planes[j].m.userptr =
        mDisplayStreamBuf.frame[i].fd;
      mDisplayBuf.preview.buf.mp[i].planes[j].data_offset = 0;
      mDisplayBuf.preview.buf.mp[i].planes[j].reserved[0] =
        mDisplayBuf.preview.buf.mp[i].planes[j-1].reserved[0] +
        mDisplayBuf.preview.buf.mp[i].planes[j-1].length;
    }

    for (int j = 0; j < num_planes; j++)
      ALOGE("Planes: %d length: %d userptr: %lu offset: %d\n", j,
        mDisplayBuf.preview.buf.mp[i].planes[j].length,
        mDisplayBuf.preview.buf.mp[i].planes[j].m.userptr,
        mDisplayBuf.preview.buf.mp[i].planes[j].reserved[0]);
  }/*end of for loop*/

 /* register the streaming buffers for the channel*/
  mDisplayBuf.ch_type = MM_CAMERA_CH_PREVIEW;
  mDisplayBuf.preview.num = mDisplayStreamBuf.num;
  mHalCamCtrl->mPreviewMemoryLock.unlock();
  ALOGE("%s:END",__func__);
  return NO_ERROR;

error:
    mHalCamCtrl->mPreviewMemoryLock.unlock();
    putBufferToSurface();

    ALOGV("%s: X", __func__);
    return ret;
}

status_t QCameraStream_preview::initPreviewOnlyBuffers()
{
  status_t ret = NO_ERROR;
  int width = 0;  /* width of channel  */
  int height = 0; /* height of channel */
  uint32_t frame_len = 0; /* frame planner length */
  int buffer_num = 4; /* number of buffers for display */
  const char *pmem_region;
  uint8_t num_planes = 0;
  uint32_t planes[VIDEO_MAX_PLANES];

  cam_ctrl_dimension_t dim;

  ALOGE("%s:BEGIN",__func__);
  memset(&mHalCamCtrl->mMetadata, 0, sizeof(camera_frame_metadata_t));
  mHalCamCtrl->mPreviewMemoryLock.lock();
  memset(&mHalCamCtrl->mNoDispPreviewMemory, 0, sizeof(mHalCamCtrl->mNoDispPreviewMemory));
  mHalCamCtrl->mPreviewMemoryLock.unlock();
  memset(&mNotifyBuffer, 0, sizeof(mNotifyBuffer));

/* get preview size, by qury mm_camera*/
  memset(&dim, 0, sizeof(cam_ctrl_dimension_t));
  ret = cam_config_get_parm(mCameraId, MM_CAMERA_PARM_DIMENSION, &dim);
  if (MM_CAMERA_OK != ret) {
    ALOGE("%s: error - can't get camera dimension!", __func__);
    ALOGE("%s: X", __func__);
    return BAD_VALUE;
  }else {
    width =  dim.display_width;
    height = dim.display_height;
  }

  ret = getBufferNoDisplay( );
  if(ret != NO_ERROR) {
    ALOGE("%s: cannot get memory from surface texture client, ret = %d", __func__, ret);
    return ret;
  }

  /* set 4 buffers for display */
  memset(&mDisplayStreamBuf, 0, sizeof(mDisplayStreamBuf));
  mHalCamCtrl->mPreviewMemoryLock.lock();
  this->mDisplayStreamBuf.num = mHalCamCtrl->mNoDispPreviewMemory.buffer_count;
  this->myMode=myMode; /*Need to assign this in constructor after translating from mask*/
  num_planes = dim.display_frame_offset.num_planes;
  for (int i = 0; i < num_planes; i++) {
    planes[i] = dim.display_frame_offset.mp[i].len;
  }
  this->mDisplayStreamBuf.frame_len = dim.display_frame_offset.frame_len;

  memset(&mDisplayBuf, 0, sizeof(mDisplayBuf));
  mDisplayBuf.preview.buf.mp = new mm_camera_mp_buf_t[mDisplayStreamBuf.num];
  if (!mDisplayBuf.preview.buf.mp) {
    ALOGE("%s Error allocating memory for mplanar struct ", __func__);
  }
  memset(mDisplayBuf.preview.buf.mp, 0,
    mDisplayStreamBuf.num * sizeof(mm_camera_mp_buf_t));

  /*allocate memory for the buffers*/
  void *vaddr = NULL;
  for(int i = 0; i < mDisplayStreamBuf.num; i++){
	  if (mHalCamCtrl->mNoDispPreviewMemory.camera_memory[i] == NULL)
		  continue;
      mDisplayStreamBuf.frame[i].fd = mHalCamCtrl->mNoDispPreviewMemory.fd[i];
      mDisplayStreamBuf.frame[i].cbcr_off = planes[0];
      mDisplayStreamBuf.frame[i].y_off = 0;
      mDisplayStreamBuf.frame[i].path = OUTPUT_TYPE_P;
      mDisplayStreamBuf.frame[i].buffer =
          (long unsigned int)mHalCamCtrl->mNoDispPreviewMemory.camera_memory[i]->data;
      mDisplayStreamBuf.frame[i].ion_alloc.len = mHalCamCtrl->mNoDispPreviewMemory.alloc[i].len;
      mDisplayStreamBuf.frame[i].ion_dev_fd = mHalCamCtrl->mNoDispPreviewMemory.main_ion_fd[i];
      mDisplayStreamBuf.frame[i].fd_data = mHalCamCtrl->mNoDispPreviewMemory.ion_info_fd[i];

    ALOGE("%s: idx = %d, fd = %d, size = %d, cbcr_offset = %d, y_offset = %d, "
      "vaddr = 0x%x", __func__, i, mDisplayStreamBuf.frame[i].fd,
      frame_len,
      mDisplayStreamBuf.frame[i].cbcr_off, mDisplayStreamBuf.frame[i].y_off,
      (uint32_t)mDisplayStreamBuf.frame[i].buffer);

    if (NO_ERROR != mHalCamCtrl->sendMappingBuf(
                        MSM_V4L2_EXT_CAPTURE_MODE_PREVIEW,
                        i,
                        mDisplayStreamBuf.frame[i].fd,
                        mHalCamCtrl->mNoDispPreviewMemory.size,
                        mCameraId, CAM_SOCK_MSG_TYPE_FD_MAPPING)) {
      ALOGE("%s: sending mapping data Msg Failed", __func__);
    }

    mDisplayBuf.preview.buf.mp[i].frame = mDisplayStreamBuf.frame[i];
    mDisplayBuf.preview.buf.mp[i].frame_offset = mDisplayStreamBuf.frame[i].y_off;
    mDisplayBuf.preview.buf.mp[i].num_planes = num_planes;

    /* Plane 0 needs to be set seperately. Set other planes
     * in a loop. */
    mDisplayBuf.preview.buf.mp[i].planes[0].length = planes[0];
    mDisplayBuf.preview.buf.mp[i].planes[0].m.userptr = mDisplayStreamBuf.frame[i].fd;
    mDisplayBuf.preview.buf.mp[i].planes[0].data_offset = 0;
    mDisplayBuf.preview.buf.mp[i].planes[0].reserved[0] =
      mDisplayBuf.preview.buf.mp[i].frame_offset;
    for (int j = 1; j < num_planes; j++) {
      mDisplayBuf.preview.buf.mp[i].planes[j].length = planes[j];
      mDisplayBuf.preview.buf.mp[i].planes[j].m.userptr =
        mDisplayStreamBuf.frame[i].fd;
      mDisplayBuf.preview.buf.mp[i].planes[j].data_offset = 0;
      mDisplayBuf.preview.buf.mp[i].planes[j].reserved[0] =
        mDisplayBuf.preview.buf.mp[i].planes[j-1].reserved[0] +
        mDisplayBuf.preview.buf.mp[i].planes[j-1].length;
    }

    for (int j = 0; j < num_planes; j++)
      ALOGE("Planes: %d length: %d userptr: %lu offset: %d\n", j,
        mDisplayBuf.preview.buf.mp[i].planes[j].length,
        mDisplayBuf.preview.buf.mp[i].planes[j].m.userptr,
        mDisplayBuf.preview.buf.mp[i].planes[j].reserved[0]);
  }/*end of for loop*/

 /* register the streaming buffers for the channel*/
  mDisplayBuf.ch_type = MM_CAMERA_CH_PREVIEW;
  mDisplayBuf.preview.num = mDisplayStreamBuf.num;
  mHalCamCtrl->mPreviewMemoryLock.unlock();
  ALOGE("%s:END",__func__);
  return NO_ERROR;

end:
  if (MM_CAMERA_OK == ret ) {
    ALOGV("%s: X - NO_ERROR ", __func__);
    return NO_ERROR;
  }

    ALOGV("%s: out of memory clean up", __func__);
  /* release the allocated memory */

  ALOGV("%s: X - BAD_VALUE ", __func__);
  return BAD_VALUE;
}


void QCameraStream_preview::dumpFrameToFile(struct msm_frame* newFrame)
{
  int32_t enabled = 0;
  int frm_num;
  uint32_t  skip_mode;
  char value[PROPERTY_VALUE_MAX];
  char buf[32];
  int w, h;
  static int count = 0;
  cam_ctrl_dimension_t dim;
  int file_fd;
  int rc = 0;
  int len;
  unsigned long addr;
  unsigned long * tmp = (unsigned long *)newFrame->buffer;
  addr = *tmp;
  status_t ret = cam_config_get_parm(mHalCamCtrl->mCameraId,
                 MM_CAMERA_PARM_DIMENSION, &dim);

  w = dim.display_width;
  h = dim.display_height;
  len = (w * h)*3/2;
  count++;
  if(count < 100) {
    snprintf(buf, sizeof(buf), "/data/mzhu%d.yuv", count);
    file_fd = open(buf, O_RDWR | O_CREAT, 0777);

    rc = write(file_fd, (const void *)addr, len);
    ALOGE("%s: file='%s', vaddr_old=0x%x, addr_map = 0x%p, len = %d, rc = %d",
          __func__, buf, (uint32_t)newFrame->buffer, (void *)addr, len, rc);
    close(file_fd);
    ALOGE("%s: dump %s, rc = %d, len = %d", __func__, buf, rc, len);
  }
}

status_t  QCameraStream_preview::convert_ycrcb420_to_yv12(mm_camera_ch_data_buf_t *frame)
{
  unsigned int dy = mHalCamCtrl->mPreviewHeight;
  unsigned int dx = mHalCamCtrl->mPreviewWidth;
  unsigned int y_size = dx*dy;
  unsigned int c_size = (dx/2)*(dy/2);
  unsigned long buffer = frame->def.frame->buffer;
  unsigned int y_off = frame->def.frame->y_off;
  unsigned int cbcr_off = frame->def.frame->cbcr_off;

  unsigned char* chroma = (unsigned char*) (buffer+y_off+cbcr_off);
  unsigned int tempbufsize = c_size*2;
  unsigned char* tempbuf = (unsigned char*)malloc(tempbufsize);
  unsigned int i = 0;
  unsigned int j = 0;
  unsigned int k = 0;

  unsigned int   stride = mHalCamCtrl->mPreviewWidth;
  unsigned int   c_width = ((stride/2 + 15)/16) * 16;
  unsigned int   c_height = mHalCamCtrl->mPreviewHeight;
  unsigned int   c_size_rounded = c_width * (c_height/2);

  ALOGI("%s, width=%d, height=%d", __func__, mHalCamCtrl->mPreviewWidth, mHalCamCtrl->mPreviewHeight);
  ALOGI("%s, y_size=%d, c_size=%d, buffer=%d, y_off=%d, cbcr_off=%d, tempbufsize=%d", __func__, y_size, c_size, buffer, y_off, cbcr_off, tempbufsize);
  memcpy(tempbuf,chroma,tempbufsize);

  if(mHalCamCtrl->mPreviewWidth%32==0)
  {
    for(i=0;i<tempbufsize/2;i++)
    {
      chroma[i] = tempbuf[2*i];
      chroma[i+tempbufsize/2] = tempbuf[2*i+1];
    }
  }
  else
  {
    for(i=0;i<c_height/2;i++)
    {
        for(j=0;j<stride/2;j++)
        {
           chroma[i*c_width+j] = tempbuf[i*stride + 2*j];
           chroma[i*c_width+c_size_rounded+j] = tempbuf[i*stride + 2*j + 1];
        }
        for(k=stride/2;k<c_width;k++) // Padding the remaining (c_width - stride/2) with zero
        {
           chroma[i*c_width+k] = 0;
           chroma[i*c_width+c_size_rounded+k] = 0;
        }
    }
  }

  if(tempbuf)
     free(tempbuf);

  return NO_ERROR;
}

status_t QCameraStream_preview::processPreviewFrameWithDisplay(
  mm_camera_ch_data_buf_t *frame)
{
  ALOGV("%s",__func__);
  int err = 0;
  int msgType = 0;
  int i;
  camera_memory_t *data = NULL;
  camera_frame_metadata_t *metadata = NULL;
  bool rcb1 = false;
  bool rcb2 = false;
  bool pcb1 = false;
  bool previewMem1 = false;

  mStopCallbackLock.lock();
  Mutex::Autolock plock(mPreviewFrameLock);
  if(!mActive) {
    ALOGE("Preview Stopped. Returning callback");
    mStopCallbackLock.unlock();
    return NO_ERROR;
  }
  if(mHalCamCtrl==NULL) {
    ALOGE("%s: X: HAL control object not set",__func__);
    /*Call buf done*/
    mStopCallbackLock.unlock();
    return BAD_VALUE;
  }
  mHalCamCtrl->mCallbackLock.lock();
  camera_data_timestamp_callback rcb = mHalCamCtrl->mDataCbTimestamp;
  void *rdata = mHalCamCtrl->mCallbackCookie;
  mHalCamCtrl->mCallbackLock.unlock();
  nsecs_t timeStamp = seconds_to_nanoseconds(frame->def.frame->ts.tv_sec) ;
  timeStamp += frame->def.frame->ts.tv_nsec;

 // if (UNLIKELY(mHalCamCtrl->mDebugFps)) {
      mHalCamCtrl->debugShowPreviewFPS();
  //}

#ifdef USE_ION

  if (vt_camera_enable)
  {
   struct mdp_blit_req* blitReq;

   union {
        char dummy[sizeof(struct mdp_blit_req_list) + sizeof(struct mdp_blit_req)*1];
        struct mdp_blit_req_list list;
    } imgFrame;


  	memset(&imgFrame, 0, sizeof(imgFrame));
    imgFrame.list.count = 1;

    blitReq = &(imgFrame.list.req[0]);
    blitReq->src.width  = mHalCamCtrl->mPreviewWidth;
    blitReq->src.height = mHalCamCtrl->mPreviewHeight;
    blitReq->src.format = MDP_Y_CBCR_H2V2;
    blitReq->src.offset = (uint32_t)0;
    blitReq->src.memory_id = frame->def.frame->fd;
    blitReq->src.priv = 0;

    blitReq->dst.width  = mHalCamCtrl->mPreviewHeight;
    blitReq->dst.height = mHalCamCtrl->mPreviewWidth;
    blitReq->dst.format = MDP_Y_CBCR_H2V2;
    blitReq->dst.offset = 0;
    blitReq->dst.memory_id = ion_fddata.fd;

    blitReq->transp_mask = 0xffffffff;
	if(mCameraId== 1)
        blitReq->flags = MDP_ROT_270;
	else if(mCameraId == 0)
        blitReq->flags = MDP_ROT_90;

    blitReq->alpha  = 0xFF;
    blitReq->dst_rect.x = 0;
    blitReq->dst_rect.y = 0;
    blitReq->dst_rect.w = mHalCamCtrl->mPreviewHeight;
    blitReq->dst_rect.h = mHalCamCtrl->mPreviewWidth;

    blitReq->src_rect.x = 0;
    blitReq->src_rect.y = 0;
    blitReq->src_rect.w = mHalCamCtrl->mPreviewWidth;
    blitReq->src_rect.h = mHalCamCtrl->mPreviewHeight;

    int erro = 0;
    if (erro = ioctl(fb_fd, MSMFB_BLIT, &imgFrame.list)) ALOGE("CSVT: MSMFB_BLIT error1 %d\n", (uint)erro);

    memset(&imgFrame, 0, sizeof(imgFrame));
    imgFrame.list.count = 1;
    blitReq = &(imgFrame.list.req[0]);
    blitReq->src.width  = mHalCamCtrl->mPreviewHeight;
    blitReq->src.height = mHalCamCtrl->mPreviewWidth;
    blitReq->src.format = MDP_Y_CBCR_H2V2;
    blitReq->src.offset = 0;
    blitReq->src.memory_id = ion_fddata.fd;
    blitReq->src.priv = 0;

    blitReq->dst.width  = mHalCamCtrl->mPreviewWidth;
    blitReq->dst.height = mHalCamCtrl->mPreviewHeight;
    blitReq->dst.format = MDP_Y_CBCR_H2V2;
    blitReq->dst.offset = 0;
    blitReq->dst.memory_id = frame->def.frame->fd;;

    blitReq->transp_mask = 0xffffffff;
	//if(mCameraId == 1)  blitReq->flags = MDP_FLIP_LR;

    blitReq->alpha  = 0xFF;
    blitReq->dst_rect.x = 0;
    blitReq->dst_rect.y = 0;
    blitReq->dst_rect.w = mHalCamCtrl->mPreviewWidth;
    blitReq->dst_rect.h = mHalCamCtrl->mPreviewHeight;

    blitReq->src_rect.x = 0;
    blitReq->src_rect.y = 0;
    blitReq->src_rect.w = mHalCamCtrl->mPreviewHeight;
    blitReq->src_rect.h = mHalCamCtrl->mPreviewWidth;

    if (erro = ioctl(fb_fd, MSMFB_BLIT, &imgFrame.list)) ALOGE("CSVT: MSMFB_BLIT error2 %d\n", (uint)erro);

  }
  #endif

  //dumpFrameToFile(frame->def.frame);
  mHalCamCtrl->dumpFrameToFile(frame->def.frame, HAL_DUMP_FRM_PREVIEW);

  mHalCamCtrl->mPreviewMemoryLock.lock();
  mNotifyBuffer[frame->def.idx] = *frame;

  ALOGD("Enqueue buf handle %p\n",
  mHalCamCtrl->mPreviewMemory.buffer_handle[frame->def.idx]);
  ALOGD("%s: camera call genlock_unlock", __FUNCTION__);
    if (BUFFER_LOCKED == mHalCamCtrl->mPreviewMemory.local_flag[frame->def.idx]) {
      ALOGD("%s: genlock_unlock_buffer hdl =%p", __FUNCTION__, (*mHalCamCtrl->mPreviewMemory.buffer_handle[frame->def.idx]));
        if (GENLOCK_FAILURE == genlock_unlock_buffer((native_handle_t*)
	            (*mHalCamCtrl->mPreviewMemory.buffer_handle[frame->def.idx]))) {
            ALOGE("%s: genlock_unlock_buffer failed", __FUNCTION__);
	        //mHalCamCtrl->mPreviewMemoryLock.unlock();
            //return -EINVAL;
        } else {
            mHalCamCtrl->mPreviewMemory.local_flag[frame->def.idx] = BUFFER_UNLOCKED;
        }
    } else {
        ALOGE("%s: buffer to be enqueued is not locked", __FUNCTION__);
	    //mHalCamCtrl->mPreviewMemoryLock.unlock();
        //return -EINVAL;
    }

#ifdef USE_ION
  struct ion_flush_data cache_inv_data;
  int ion_fd;
  ion_fd = frame->def.frame->ion_dev_fd;
  cache_inv_data.vaddr = (void *)frame->def.frame->buffer;
  cache_inv_data.fd = frame->def.frame->fd;
  cache_inv_data.handle = frame->def.frame->fd_data.handle;
  cache_inv_data.length = frame->def.frame->ion_alloc.len;

  if (mHalCamCtrl->cache_ops(ion_fd, &cache_inv_data, ION_IOC_CLEAN_CACHES) < 0)
    ALOGE("%s: Cache clean for Preview buffer %p fd = %d failed", __func__,
      cache_inv_data.vaddr, cache_inv_data.fd);
#endif

  if(mHalCamCtrl->mPreviewFormat == CAMERA_YUV_420_YV12) {
    convert_ycrcb420_to_yv12(frame);
   }


  if(mHFRFrameSkip == 1)
  {
      const char *str = mHalCamCtrl->mParameters.get(
                          QCameraParameters::KEY_QC_VIDEO_HIGH_FRAME_RATE);
      if(str != NULL){
      int is_hfr_off = 0;
      mHFRFrameCnt++;
      if(!strcmp(str, QCameraParameters::VIDEO_HFR_OFF)) {
          is_hfr_off = 1;
          err = this->mPreviewWindow->enqueue_buffer(this->mPreviewWindow,
            (buffer_handle_t *)mHalCamCtrl->mPreviewMemory.buffer_handle[frame->def.idx]);
      } else if (!strcmp(str, QCameraParameters::VIDEO_HFR_2X)) {
          mHFRFrameCnt %= 2;
      } else if (!strcmp(str, QCameraParameters::VIDEO_HFR_3X)) {
          mHFRFrameCnt %= 3;
      } else if (!strcmp(str, QCameraParameters::VIDEO_HFR_4X)) {
          mHFRFrameCnt %= 4;
      }
      if(mHFRFrameCnt == 0)
          err = this->mPreviewWindow->enqueue_buffer(this->mPreviewWindow,
            (buffer_handle_t *)mHalCamCtrl->mPreviewMemory.buffer_handle[frame->def.idx]);
      else if(!is_hfr_off)
          err = this->mPreviewWindow->cancel_buffer(this->mPreviewWindow,
            (buffer_handle_t *)mHalCamCtrl->mPreviewMemory.buffer_handle[frame->def.idx]);
      } else
          err = this->mPreviewWindow->enqueue_buffer(this->mPreviewWindow,
            (buffer_handle_t *)mHalCamCtrl->mPreviewMemory.buffer_handle[frame->def.idx]);
  } else {
      err = this->mPreviewWindow->enqueue_buffer(this->mPreviewWindow,
          (buffer_handle_t *)mHalCamCtrl->mPreviewMemory.buffer_handle[frame->def.idx]);
  }
  if(err != 0) {
    ALOGE("%s: enqueue_buffer failed, err = %d", __func__, err);
  } else {
   ALOGD("%s: enqueue_buffer hdl=%p", __func__, *mHalCamCtrl->mPreviewMemory.buffer_handle[frame->def.idx]);
    mHalCamCtrl->mPreviewMemory.local_flag[frame->def.idx] = BUFFER_NOT_OWNED;
  }
  buffer_handle_t *buffer_handle = NULL;
  int tmp_stride = 0;
  err = this->mPreviewWindow->dequeue_buffer(this->mPreviewWindow,
              &buffer_handle, &tmp_stride);
  if (err == NO_ERROR && buffer_handle != NULL) {

    ALOGD("%s: dequed buf hdl =%p", __func__, *buffer_handle);
    for(i = 0; i < mHalCamCtrl->mPreviewMemory.buffer_count; i++) {
        if(mHalCamCtrl->mPreviewMemory.buffer_handle[i] == buffer_handle) {
          mHalCamCtrl->mPreviewMemory.local_flag[i] = BUFFER_UNLOCKED;
          break;
        }
    }
     if (i < mHalCamCtrl->mPreviewMemory.buffer_count ) {
      err = this->mPreviewWindow->lock_buffer(this->mPreviewWindow, buffer_handle);
      ALOGD("%s: camera call genlock_lock: hdl =%p", __FUNCTION__, *buffer_handle);
      if (GENLOCK_FAILURE == genlock_lock_buffer((native_handle_t*)(*buffer_handle), GENLOCK_WRITE_LOCK,
                                                 GENLOCK_MAX_TIMEOUT)) {
            ALOGE("%s: genlock_lock_buffer(WRITE) failed", __FUNCTION__);
	    //mHalCamCtrl->mPreviewMemoryLock.unlock();
           // return -EINVAL;
      } else  {
        mHalCamCtrl->mPreviewMemory.local_flag[i] = BUFFER_LOCKED;

        if(MM_CAMERA_OK != cam_evt_buf_done(mCameraId, &mNotifyBuffer[i])) {
            ALOGE("BUF DONE FAILED");
        }
      }
     }
  } else
      ALOGE("%s: error in dequeue_buffer, enqueue_buffer idx = %d, no free buffer now", __func__, frame->def.idx);
  /* Save the last displayed frame. We'll be using it to fill the gap between
     when preview stops and postview start during snapshot.*/
  mLastQueuedFrame = &(mDisplayStreamBuf.frame[frame->def.idx]);
  mHalCamCtrl->mPreviewMemoryLock.unlock();

  mHalCamCtrl->mCallbackLock.lock();
  camera_data_callback pcb = mHalCamCtrl->mDataCb;
  mHalCamCtrl->mCallbackLock.unlock();
  ALOGD("Message enabled = 0x%x", mHalCamCtrl->mMsgEnabled);

  camera_memory_t *previewMem = NULL;

  if (pcb != NULL) {
       ALOGD("%s: mMsgEnabled =0x%x, preview format =%d", __func__,
            mHalCamCtrl->mMsgEnabled, mHalCamCtrl->mPreviewFormat);
      //Sending preview callback if corresponding Msgs are enabled
      if(mHalCamCtrl->mMsgEnabled & CAMERA_MSG_PREVIEW_FRAME) {
          ALOGE("Q%s: PCB callback enabled", __func__);
          msgType |=  CAMERA_MSG_PREVIEW_FRAME;
          int previewBufSize;
          /* The preview buffer size sent back in the callback should be (width*height*bytes_per_pixel)
           * As all preview formats we support, use 12 bits per pixel, buffer size = previewWidth * previewHeight * 3/2.
           * We need to put a check if some other formats are supported in future. (punits) */
          if((mHalCamCtrl->mPreviewFormat == CAMERA_YUV_420_NV21) || (mHalCamCtrl->mPreviewFormat == CAMERA_YUV_420_NV12) ||
                    (mHalCamCtrl->mPreviewFormat == CAMERA_YUV_420_YV12))
          {
            if(mHalCamCtrl->mPreviewFormat == CAMERA_YUV_420_YV12) {
              previewBufSize = ((mHalCamCtrl->mPreviewWidth+15)/16) *16* mHalCamCtrl->mPreviewHeight +
                ((mHalCamCtrl->mPreviewWidth/2+15)/16)*16* mHalCamCtrl->mPreviewHeight;
            } else {
              previewBufSize = mHalCamCtrl->mPreviewWidth * mHalCamCtrl->mPreviewHeight * 3/2;
            }
              if(previewBufSize != mHalCamCtrl->mPreviewMemory.private_buffer_handle[frame->def.idx]->size) {
                  previewMem = mHalCamCtrl->mGetMemory(mHalCamCtrl->mPreviewMemory.private_buffer_handle[frame->def.idx]->fd,
                  previewBufSize, 1, mHalCamCtrl->mCallbackCookie);
                  if (!previewMem || !previewMem->data) {
                      ALOGE("%s: mGetMemory failed.\n", __func__);
                  } else {
                      data = previewMem;
                  }
              } else
                    data = mHalCamCtrl->mPreviewMemory.camera_memory[frame->def.idx];
          } else {
                data = mHalCamCtrl->mPreviewMemory.camera_memory[frame->def.idx];
                ALOGE("Invalid preview format, buffer size in preview callback may be wrong.");
          }
      } else {
          data = NULL;
      }
      if(msgType) {
          if(mActive)
              pcb1 = true;
          if (previewMem)
              previewMem1 = true;
      }
	  ALOGD("end of cb");
  } else {
    ALOGD("%s PCB is not enabled", __func__);
  }
  int flagwait = 0;
  if(rcb != NULL && mVFEOutputs == 1)
  {
      if(mHalCamCtrl->mStartRecording == true &&
              ( mHalCamCtrl->mMsgEnabled & CAMERA_MSG_VIDEO_FRAME))
      {
        if (mHalCamCtrl->mStoreMetaDataInFrame)
        {
          if(mHalCamCtrl->mRecordingMemory.metadata_memory[frame->def.idx])
          {
              flagwait = 1;
              rcb1=true;
          }else
              flagwait = 0;
      }else{
              rcb2=true;
              flagwait = 1;
      }
      }
  }

    mStopCallbackLock.unlock();
    if(pcb1 == true) {
        pcb(msgType, data, 0, metadata, mHalCamCtrl->mCallbackCookie);
    }
    if(previewMem1 == true) {
        previewMem->release(previewMem);
    }
    if(rcb1 == true) {
        mHalCamCtrl->mRecordLock.lock();
        if(mHalCamCtrl->mStartRecording == true && mHalCamCtrl->mRecordingMemory.metadata_memory[frame->def.idx]) {
          rcb(timeStamp, CAMERA_MSG_VIDEO_FRAME,
              mHalCamCtrl->mRecordingMemory.metadata_memory[frame->def.idx],
              0, mHalCamCtrl->mCallbackCookie);
        }
        else
          flagwait = 0;
        mHalCamCtrl->mRecordLock.unlock();
        ALOGD("calling rcb metadata");
    }
    if(rcb2 == true) {
        rcb(timeStamp, CAMERA_MSG_VIDEO_FRAME,
            mHalCamCtrl->mPreviewMemory.camera_memory[frame->def.idx],
            0, mHalCamCtrl->mCallbackCookie);
    }

    if(flagwait){
        Mutex::Autolock rLock(&mHalCamCtrl->mRecordFrameLock);
        if(mHalCamCtrl->mReleasedRecordingFrame != true) {
            mHalCamCtrl->mRecordWait.wait(mHalCamCtrl->mRecordFrameLock);
        }
        mHalCamCtrl->mReleasedRecordingFrame = false;
    }

  /* Save the last displayed frame. We'll be using it to fill the gap between
     when preview stops and postview start during snapshot.*/
  //mLastQueuedFrame = frame->def.frame;
/*
  if(MM_CAMERA_OK != cam_evt_buf_done(mCameraId, frame))
  {
      ALOGE("BUF DONE FAILED");
      return BAD_VALUE;
  }
*/
  return NO_ERROR;
}


status_t QCameraStream_preview::processPreviewFrameWithOutDisplay(
  mm_camera_ch_data_buf_t *frame)
{
  ALOGV("%s",__func__);
  int err = 0;
  int msgType = 0;
  int i;
  camera_memory_t *data = NULL;
  camera_frame_metadata_t *metadata = NULL;

  Mutex::Autolock lock(mStopCallbackLock);
  if(!mActive) {
    ALOGE("Preview Stopped. Returning callback");
    return NO_ERROR;
  }
  if(mHalCamCtrl==NULL) {
    ALOGE("%s: X: HAL control object not set",__func__);
    /*Call buf done*/
    return BAD_VALUE;
  }

  if (UNLIKELY(mHalCamCtrl->mDebugFps)) {
      mHalCamCtrl->debugShowPreviewFPS();
  }
  //dumpFrameToFile(frame->def.frame);
  mHalCamCtrl->dumpFrameToFile(frame->def.frame, HAL_DUMP_FRM_PREVIEW);

  mHalCamCtrl->mPreviewMemoryLock.lock();
  mNotifyBuffer[frame->def.idx] = *frame;

  /* Save the last displayed frame. We'll be using it to fill the gap between
     when preview stops and postview start during snapshot.*/
  mLastQueuedFrame = &(mDisplayStreamBuf.frame[frame->def.idx]);
  mHalCamCtrl->mPreviewMemoryLock.unlock();

  mHalCamCtrl->mCallbackLock.lock();
  camera_data_callback pcb = mHalCamCtrl->mDataCb;
  mHalCamCtrl->mCallbackLock.unlock();
  ALOGD("Message enabled = 0x%x", mHalCamCtrl->mMsgEnabled);

  camera_memory_t *previewMem = NULL;
  int previewWidth, previewHeight;
  mHalCamCtrl->mParameters.getPreviewSize(&previewWidth, &previewHeight);

#ifdef USE_ION
  struct ion_flush_data cache_inv_data;
  int ion_fd;
  ion_fd = frame->def.frame->ion_dev_fd;
  cache_inv_data.vaddr = (void *)frame->def.frame->buffer;
  cache_inv_data.fd = frame->def.frame->fd;
  cache_inv_data.handle = frame->def.frame->fd_data.handle;
  cache_inv_data.length = frame->def.frame->ion_alloc.len;

  if (mHalCamCtrl->cache_ops(ion_fd, &cache_inv_data, ION_IOC_CLEAN_CACHES) < 0)
    ALOGE("%s: Cache clean for Preview buffer %p fd = %d failed", __func__,
      cache_inv_data.vaddr, cache_inv_data.fd);
#endif

  if (pcb != NULL) {
      //Sending preview callback if corresponding Msgs are enabled
      if(mHalCamCtrl->mMsgEnabled & CAMERA_MSG_PREVIEW_FRAME) {
          msgType |=  CAMERA_MSG_PREVIEW_FRAME;
          int previewBufSize;
          /* For CTS : Forcing preview memory buffer lenth to be
             'previewWidth * previewHeight * 3/2'.
              Needed when gralloc allocated extra memory.*/
          //Can add this check for other formats as well.
        data = mHalCamCtrl->mNoDispPreviewMemory.camera_memory[frame->def.idx];//mPreviewHeap->mBuffers[frame->def.idx];
      } else {
          data = NULL;
      }

      if(mHalCamCtrl->mMsgEnabled & CAMERA_MSG_PREVIEW_METADATA){
          msgType  |= CAMERA_MSG_PREVIEW_METADATA;
          metadata = &mHalCamCtrl->mMetadata;
      } else {
          metadata = NULL;
      }
      if(msgType) {
          mStopCallbackLock.unlock();
          if(mActive)
            pcb(msgType, data, 0, metadata, mHalCamCtrl->mCallbackCookie);
          if (previewMem)
              previewMem->release(previewMem);
      }

      if(MM_CAMERA_OK != cam_evt_buf_done(mCameraId, &mNotifyBuffer[frame->def.idx])) {
          ALOGE("BUF DONE FAILED");
      }

      ALOGD("end of cb");
  }

  return NO_ERROR;
}
/************************************************************************************************************/
typedef struct
{
unsigned int BlackCoordinateX;//第一次发现黑点的坐标
unsigned int BlackCoordinateY;
unsigned int BlackFreamCount;//同个x坐标下，帧数统计
bool ThisIsFirstFind;//是否是第一次发现，区别在于，若是第一次寻找黑点，需逐个遍历，否这直接判断对应的纵行。
}SplitScreenInfo;
static SplitScreenInfo LongitudinalInformationRemember={300,0,0,true};
static pthread_t thread_DetermineImageSplitScreenID = NULL;
static bool globao_mFlyPreviewStatus;
static bool global_need_rePreview = false;
static bool ToFindBlackLineAndSetTheTw9912VerticalDelayRegister_is_ok = false;
static bool DetermineImageSplitScreen_do_not_or_yes = true;
static int global_fram_count = 0;
static nsecs_t DVDorAUX_last_time = 0;
static nsecs_t Astern_last_time = 0;
static unsigned int rePreview_count = 0;
static bool global_Fream_is_first = true;
static unsigned int open_dev_fail_count=0;

void *CameraRestartPreviewThread(void *mHalCamCtrl1)
{
	QCameraHardwareInterface *mHalCamCtrl = (QCameraHardwareInterface *)mHalCamCtrl1;
	usleep(1000*500);//500 ms
	if(globao_mFlyPreviewStatus == 1)
	{
		ALOGE("Flyvideo-DetermineImageSplitScreen:stopPreview()-->\n");
		mHalCamCtrl->stopPreview();
		//	sleep(1);

		ALOGE("Flyvideo-DetermineImageSplitScreen:startPreview()-->\n");
		mHalCamCtrl->startPreview();
		//	sleep(1);
	}
	else
	{
	ALOGE("Flyvideo-发现分屏需要重新preview，但是视频HAL调用啦stop。\n");
	}

	LongitudinalInformationRemember.ThisIsFirstFind = true;
	global_need_rePreview = false;
    return NULL;
}
static bool RowsOfDataTraversingTheFrameToFindTheBlackLine(mm_camera_ch_data_buf_t *frame)
{
	unsigned char *piont_y;
	int i = 0,jj =0;
	unsigned int count;
			Debug_camera("Flyvideo-可能发生分屏，寻找黑边中。。。");
			for(i=5;i<477;i++)//某列下的第几行，找 一个点 看数据是否是黑色；前10行和后10行放弃找，正常情况下前3行是黑色的数据
			{
				piont_y = (unsigned char *)(frame->def.frame->buffer+frame->def.frame->y_off+720*i + 300);//在第300列下的每行找黑点
				if(*piont_y <= 0x20)//到此，在某列下的某行，找到啦一个黑点，接下来对这一行，遍历700个点，看这行是否确实都是黑色数据
				{
					for(jj=0;jj<719;jj++)//在一行的遍历
					{
						piont_y = (unsigned char *)(frame->def.frame->buffer+frame->def.frame->y_off+720*i + jj);
						if(*piont_y <= 0x20) count++;//黑点
						if( (719-jj+count)< 715)//一定无法达到700个点的要求，没必要再执行下去。
						{//如果剩下的（715-jj）加上以发现的（count）已经小于700，将结束循环
							goto Break_The_For;
						}
					}
					Break_The_For:
					if(count >= 715) goto Break_The_Func;//当这行的黑色数据确实有700个，跳出整个遍历，返回分屏信息。
					else count =0;
				}
			}
//Debug_camera("Astern：经确认未发生黑屏");
Debug_camera("Flyvideo-找不到黑边");
return 0;
Break_The_Func:
Debug_camera("Flyvideo-：确定发生啦分屏,黑边位置在第 %d 行，发现的黑色数据个数= %d 个,其中一个黑色数据是0x%.2x",i,count,*piont_y);
return 1;//确定这帧是出现啦分屏
}
static bool VideoItselfBlackJudge(mm_camera_ch_data_buf_t *frame)
{
	unsigned int black_count;
	int i =0 ,j =0;
	unsigned char *piont_y;
	for(;j<480;j++)
		{i++;
			piont_y = (unsigned char *)(frame->def.frame->buffer+frame->def.frame->y_off+720*j + i);//取值 正方形 斜线查找
			if(*piont_y <= 0x20)//找到啦一个黑色点
				black_count ++;

			if( (480 - j + black_count) < 100)//一定无法达到470个点的要求，没必要再执行下去。
						continue;

			if(black_count>100)
				{
					ALOGE("Flyvideo-:这帧本身是黑色");
					return 1;//这帧本身是黑色
				}
		}
return 0;
}

//Determine whether the split-screen
static bool DetermineImageSplitScreen_Longitudinal(mm_camera_ch_data_buf_t *frame,QCameraHardwareInterface *mHalCamCtrl,float flymFps,char *video_channel_status,nsecs_t flynow)
{
	unsigned int i =0 ,j =0;
	unsigned char *piont_y;
	unsigned int dete_count = 0;
	for(j=20;j<700;j++)
		{
			if(LongitudinalInformationRemember.ThisIsFirstFind == false && j != LongitudinalInformationRemember.BlackCoordinateX)
				continue;//第一次寻找黑点要遍历本行所有点，否则其他行跳过

			piont_y = (unsigned char *)(frame->def.frame->buffer+frame->def.frame->y_off+720*300 + j);//在300行，横向扫描
			if(*piont_y <= 0x20)//找到啦一个黑色点
			{
				for(i=10;i<470;i++)
				{
					piont_y = (unsigned char *)(frame->def.frame->buffer+frame->def.frame->y_off+720*i + j);
					if((*piont_y) <= 0x20)
					{
						dete_count++;
						if( (460 - (i-10) + dete_count) < 455)//一定无法达到400个点的要求，没必要再执行下去。
							continue;

						if(dete_count > 455 && VideoItselfBlackJudge(frame) == 0 )//在这个纵向行黑色点个数达到啦设置值,且这帧本身不是黑色屏
						{
						//LOGE("DetermineImageSplitScreen:buf[%d]= 0x%.2x\n",((180*j) + i),(*piont_crcb) );
							if(LongitudinalInformationRemember.ThisIsFirstFind == true)//这里是第一次找到黑色点
							{
								LongitudinalInformationRemember.ThisIsFirstFind = false;
								LongitudinalInformationRemember.BlackCoordinateX = j;
								LongitudinalInformationRemember.BlackFreamCount = 1;
							}
							else
							{
								LongitudinalInformationRemember.BlackFreamCount ++;
							}
							ALOGE("Flyvideo-:纵向分屏判断，黑色数据在第%d纵行中，异常数据=%d，统计帧数=%d\n",j,dete_count,LongitudinalInformationRemember.BlackFreamCount);
							if(LongitudinalInformationRemember.BlackFreamCount  >250)//5sec
							{
								global_need_rePreview = false;//防止CameraRestartPreviewThread阻塞导致，global_need_rePreview无法赋值成true；
								LongitudinalInformationRemember.ThisIsFirstFind = true;
							}
							if(global_need_rePreview == false )
							{
								if(LongitudinalInformationRemember.BlackFreamCount >= 25) //发现有 25 帧就重新preview
								{
									global_need_rePreview = true;
									LongitudinalInformationRemember.BlackFreamCount = 0;
									//LOGE("Flyvideo-: need rePreview\n");

									if( pthread_create(&thread_DetermineImageSplitScreenID, NULL,CameraRestartPreviewThread, (void *)mHalCamCtrl) != 0)
										ALOGE("Flyvideo-纵向分屏判断,发现分屏，创建线程重新预览失败！\n");
									else
										ALOGE("Flyvideo-纵向分屏判断,发现分屏，创建线程重新预览成功！\n");
									return 1;//返回分屏信息
								}
							}
						goto BREAK_THE;
						}
					}

				}
			}
		}
BREAK_THE:
return 0;//未发生分屏
}
static bool DetermineImageSplitScreen(mm_camera_ch_data_buf_t *frame,QCameraHardwareInterface *mHalCamCtrl,float flymFps,char *video_channel_status,nsecs_t flynow)
{
	int i =0 ,j =0;
    //static char video_channel_status[10]="1"; //1:DVD 2:AUX 3:Astren
	unsigned char *piont_y;
	unsigned char *piont_crcb;
    static int global_fram_count = 0;
    unsigned int dete_count = 0;

	if(flymFps<24){ALOGE("Flyvideo-:flymFps = %f",flymFps);return 0;}

	//纵向分屏判断
	//if(DetermineImageSplitScreen_Longitudinal(frame,mHalCamCtrl,flymFps,video_channel_status,flynow) == 1)
	//	return 1;

	// property_get("fly.video.channel.status",video_channel_status,"1");//1:DVD 2:AUX 3:Astren

	/****************************Astren******************************/
	if(video_channel_status[0] == '3')//Astren
	//if(0)
	{//ALOGE("DetermineImageSplitScreen:Astren");
		if(DetermineImageSplitScreen_do_not_or_yes == false)
		{
			return 0;//未发生分屏
		}
		else
		{
			for(j=0;j<4;j++)
			{
				for(i=0;i<20;i++)
				{
					piont_y = (unsigned char *)(frame->def.frame->buffer+frame->def.frame->y_off+180*j + i);
					if((*piont_y) < 0x20 || (*piont_y) > 0x35 )
					{
						dete_count++;//一行判断超过10个可疑点才认为是数据异常
							if(dete_count > 5)
							{//Debug_camera("Flyvideo-异常数据：buf[%d]= 0x%.2x\n",((180*j) + i),(*piont_y) );
							//ALOGE("DetermineImageSplitScreen:buf[%d]= 0x%.2x\n",((180*j) + i),(*piont_crcb) );
							global_fram_count ++;//数据异常帧数统计
								if(global_fram_count >50)//1sec
								{
									global_need_rePreview = false;//防止CameraRestartPreviewThread阻塞导致，global_need_rePreview无法赋值成true；
									global_fram_count = 0;
								}
								if(global_need_rePreview == false && (flynow - Astern_last_time) > ms2ns(2500))//离上次时间超过2.5s
								{
									Astern_last_time = flynow;
									if(global_fram_count >= 1 && RowsOfDataTraversingTheFrameToFindTheBlackLine(frame)) //发现有一帧就重新preview
									{
										global_fram_count = 0;
										global_need_rePreview = true;
										//ALOGE("Flyvideo-: need rePreview\n");

										if( pthread_create(&thread_DetermineImageSplitScreenID, NULL,CameraRestartPreviewThread, (void *)mHalCamCtrl) != 0)
											ALOGE("Flyvideo-发现分屏，创建线程重新预览失败！Astren\n");
										else
											ALOGE("Flyvideo-发现分屏，创建线程重新预览成功！Astren\n");
										return 1;//返回分屏信息
									}
								}
							goto BREAK_THE;
							}
					}
				}
			}
		}
	}
	/****************************DVD or AUX******************************/
	else
	{
		//ALOGE("DetermineImageSplitScreen:DVD or AUX");
		for(j=0;j<4;j++)
		{
			for(i=0;i<20;i++)
			{
				piont_crcb = (unsigned char *)(frame->def.frame->buffer+frame->def.frame->cbcr_off+(180*j + i));
				if((*piont_crcb) < (0x7f - 10) || (*piont_crcb) > (0x80+10) )//第一行有颜色说明发生啦分屏
				{
					dete_count++;//一行判断超过10个可疑点才认为是数据异常
						if(dete_count > 9)
						{
						Debug_camera("Flyvideo-异常数据积累个数达到啦设置值");
						//ALOGE("DetermineImageSplitScreen:buf[%d]= 0x%.2x\n",((180*j) + i),(*piont_crcb) );
						global_fram_count ++;//数据异常帧数统计
							if(global_fram_count >50)//1sec
							{
								global_need_rePreview = false;//防止CameraRestartPreviewThread阻塞导致，global_need_rePreview无法赋值成true；
								global_fram_count = 0;
							}
							if(global_need_rePreview == false && (flynow - DVDorAUX_last_time) > ms2ns(15000))//离上次时间超过10s
							{
								DVDorAUX_last_time = flynow;
								if(global_fram_count >= 1 && RowsOfDataTraversingTheFrameToFindTheBlackLine(frame)) //发现有一帧就重新preview ,且寻找到啦黑边
								{
									global_fram_count = 0;
									global_need_rePreview = true;
									//ALOGE("Flyvideo-: need rePreview\n");

									if( pthread_create(&thread_DetermineImageSplitScreenID, NULL,CameraRestartPreviewThread, (void *)mHalCamCtrl) != 0)
										ALOGE("Flyvideo-发现分屏，创建线程重新预览失败！DVD or AUX\n");
									else
										ALOGE("Flyvideo-发现分屏，创建线程重新预览成功！DVD or AUX\n");
									return 1;//返回分屏信息
								}
							}
						goto BREAK_THE;
						}
				}

			}

		}
	}
BREAK_THE:
if(video_channel_status[0] == '2' || video_channel_status[0] == '3')//AUX
	memset((void *)(frame->def.frame->buffer+frame->def.frame->y_off),0,720*3);
return 0;//未发生分屏
}
static void FlagBlack(mm_camera_ch_data_buf_t *frame)//填充白色
{
int i,j;
static int NumberFlag=0;
for(j=100;j<120;j++)
	for(i=NumberFlag*100;i<=(NumberFlag*100+20);i+=4)
	{
	*(unsigned long *)(frame->def.frame->buffer+frame->def.frame->y_off+j*720+i) |=0xffffffff;
	}
NumberFlag++;
if(NumberFlag>7)NumberFlag=0;
}
static bool BlackJudge(mm_camera_ch_data_buf_t *frame)
{
unsigned char *piont_y,*piont_y_last;
unsigned int i,j,i_last=0;
unsigned int black_count=0;
//FlagBlack(frame);
//网格判断
//	for(j=477;j<479;j++)//第几行
//	{
		for(i=0;i<719;i++)//第几列
		{
				piont_y = (unsigned char *)(frame->def.frame->buffer+frame->def.frame->y_off+475*720+ i);
				//piont_y_last = (unsigned char *)(frame->def.frame->buffer+frame->def.frame->y_off+j*720+ i_last);
				if(!i) piont_y_last = piont_y;

				//if( (*piont_y) == (*piont_y_last) && (*piont_y) < 0x25)//TW9912 黑屏数据是 0x20
				if( (*piont_y) == (*piont_y_last) && (*piont_y) < 0x2f)//TW9912 黑屏数据是 0x20
				{
					black_count++;
					if( (719-i+black_count)< 280)//一定无法达到700个点的要求，没必要再执行下去。
					return 0;
					if( black_count > 280)//“黑色"数据大于700认为是黑屏
					{
						//FlagBlack(frame);
						Debug_camera("Flyvideo-：确定倒车视频已断开,发现的黑色数据个数= %d 个,其中一个黑色数据是0x%.2x",black_count,*piont_y);
						return 1;
					}
				}
				//else return 0;
				i_last =i;
				piont_y_last = piont_y;
		}
	//}
return 0;
}
static bool ToFindBlackLine(mm_camera_ch_data_buf_t *frame)
{
	unsigned char *piont_y,*piont_y_last;
	unsigned int i,j,i_last=0;
	unsigned int black_count=0;
		for(i=0;i<719;i++)//第几列
		{
				piont_y = (unsigned char *)(frame->def.frame->buffer+frame->def.frame->y_off+ i);//第一行
				if(!i) piont_y_last = piont_y;

				//if( (*piont_y) == (*piont_y_last) && (*piont_y) < 0x25)//TW9912 黑屏数据是 0x20
				//if( (*piont_y) == (*piont_y_last) && (*piont_y) < 0x2f)//TW9912 黑屏数据是 0x20
				if((*piont_y) >= 0x20 && (*piont_y) <= 0x35 )
				{
					black_count++;
					if( (719-i+black_count)< 700)//一定无法达到700个点的要求，没必要再执行下去。
					return 0;
					if( black_count > 700)//“黑色"数据大于700认为是黑线出现
					{
						//FlagBlack(frame);
						Debug_camera("Flyvideo-：第一行黑色行，成功腾出,在这行黑色数据的个数 = %d 个,其中一个黑色数据是0x%.2x",black_count,*piont_y);
						return 1;
					}
				}
				//else return 0;
				i_last =i;
				piont_y_last = piont_y;
		}
	return 0;
}
typedef struct
{
unsigned char reg;
unsigned char reg_val;
bool sta;//true is find black line;
bool flag;//true is neet again find the black line;
bool this_is_first_open;//true is first open AND is open DVD
}TW9912Info;

static int ToFindBlackLineAndSetTheTw9912VerticalDelayRegister(mm_camera_ch_data_buf_t *frame)
{
	int file_fd;
	TW9912Info tw9912_info;
	file_fd = open("/dev/tw9912config",O_RDWR);
		if(file_fd ==-1)
		{
				if(open_dev_fail_count == 0)
				{
						ALOGE("Flyvideo-错误x，打开设备节点文件“/dev/tw9912config”失败");
						//ToFindBlackLineAndSetTheTw9912VerticalDelayRegister_is_ok = true;
						ALOGE("Flyvideo-警告：重要！！由于节点/dev/tw9912config打开失败，相关配置无法完成，分屏判断将失效，请检查该节点的读写权限（>606）");
						DetermineImageSplitScreen_do_not_or_yes = false;
						open_dev_fail_count ++;
						goto OPEN_ERR;
						//return -1;
				}
				else if( open_dev_fail_count >60 )
				{
						open_dev_fail_count = 1;
						if(file_fd ==-1)
						{
							ALOGE("Flyvideo-错误`，打开设备节点文件“/dev/tw9912config”失败");
							//ToFindBlackLineAndSetTheTw9912VerticalDelayRegister_is_ok = true;
							ALOGE("Flyvideo-警告：重要！！由于节点/dev/tw9912config打开失败，相关配置无法完成，分屏判断将失效，请检查该节点的读写权限（>606）");
							DetermineImageSplitScreen_do_not_or_yes = false;
							goto OPEN_ERR;
							//return -1;
						}
				}
				else
				{
					open_dev_fail_count ++;
					goto OPEN_ERR;
				}
		}

	read(file_fd, (void *)(&tw9912_info),sizeof(TW9912Info));
	if(tw9912_info.flag == true)
	{
		if(ToFindBlackLine(frame) ==1)//找到啦黑色数据行
		{
			tw9912_info.sta = true;
			tw9912_info.flag = false;
			write(file_fd, (const void *)(&tw9912_info),sizeof(TW9912Info));
		ALOGE("Flyvideo-找到了黑色数据行，图像下移策略不需再执行");
		ToFindBlackLineAndSetTheTw9912VerticalDelayRegister_is_ok = true;
		DetermineImageSplitScreen_do_not_or_yes = true;
		}
		else//还没找到黑色数据行 图像下移一行
		{
			if(tw9912_info.reg_val > 0x10 && tw9912_info.reg_val <= 0x17)
			{
				tw9912_info.reg = 0x08;
				tw9912_info.reg_val -= 1;
				write(file_fd, (const void *)(&tw9912_info),sizeof(TW9912Info));
				ALOGE("Flyvideo-黑色数据行还未找到，图像下移一个像素点");
			}
			else
			{
				ALOGE("Flyvideo-警告：图像顶部腾出的黑线还未找到，但是图像下移数据的设置已经达到下限，不能再设置更小");
				ToFindBlackLineAndSetTheTw9912VerticalDelayRegister_is_ok = true;
				ALOGE("Flyvideo-警告：重要！！由于腾出黑线的操作无法完成，分屏判断将失效");
				DetermineImageSplitScreen_do_not_or_yes = false;
			}
		return 1;
		}

	}
	else
	{
		ALOGE("Flyvideo-从内核读回的数据 tw9912_info.flag == false 无需再“腾”顶部的黑线");
		ToFindBlackLineAndSetTheTw9912VerticalDelayRegister_is_ok = true;
	}
	close(file_fd);
OPEN_ERR:
return 0;
}
status_t QCameraStream_preview::processPreviewFrame (
  mm_camera_ch_data_buf_t *frame)
{
	static int index = 0;
	static char pal_enabel[10]="1";
	static char video_channel_status[10]="0";
	static unsigned int frame_count_last = 0;
	static unsigned int frame_count_now = 0;

	char video_show_status[10]="0";

    static int flymFrameCount;
    static int flymLastFrameCount = 0;
    static nsecs_t flymLastFpsTime = 0;
    static float flymFps = 0;
	float FrameCount_flag = 40.0;
    flymFrameCount++;
    nsecs_t flynow = systemTime();
    nsecs_t flydiff = flynow - flymLastFpsTime;
    if (flydiff > ms2ns(250)) {
        flymFps =  ((flymFrameCount - flymLastFrameCount) * float(s2ns(1))) / flydiff;
        //ALOGE("Preview Frames Per Second fly: %.4f", flymFps);
        flymLastFpsTime = flynow;
        flymLastFrameCount = flymFrameCount;
    }
    property_get("fly.video.show.status",video_show_status,"1");// 1 is show normal 0 is show black
    property_get("fly.video.channel.status",video_channel_status,"1");//1:DVD 2:AUX 3:Astren
    property_get("tcc.fly.vin.pal",pal_enabel,"1");// 1 is pal 0 is ntsc

/*
									DVD : NTSC PAL 
										   1    2
								CAM or AUX: NTSC PAL
									         3	  4							*/
/*
if(video_show_status[0] == '0')
{
ALOGE("上次视频打开，无视频\n");
}
else
{
ALOGE("上次视频打开，正常\n");
}
*/
if(video_channel_status[0] == '1')
FrameCount_flag = 60.0;
else
FrameCount_flag = 40.0;
if(video_channel_status[0] == '3' && ToFindBlackLineAndSetTheTw9912VerticalDelayRegister_is_ok == false)//onle at Astren and not PAL
	{
		#if 0
		if( (++frame_count_now) - frame_count_last > 25 )
		{
		frame_count_last = frame_count_now;
		ToFindBlackLineAndSetTheTw9912VerticalDelayRegister(frame);
		}
		#else
		if(ToFindBlackLineAndSetTheTw9912VerticalDelayRegister(frame) == 1)
			{
			ALOGE("Flyvideo-注意：图像静帧一帧");
			return processPreviewFrameWithOutDisplay(frame);
			}
		#endif
	}
if(video_channel_status[0] == '1' && global_Fream_is_first == true)
   {
           int file_fd;
           TW9912Info tw9912_info;
           file_fd = open("/dev/tw9912config",O_RDWR);
           if(file_fd ==-1)
           {
                   ALOGE("Flyvideo-错误，打开设备节点文件“/dev/tw9912config”失败");
                  // return -1;
										goto OPEN_ERR;
           }
           read(file_fd, (void *)(&tw9912_info),sizeof(TW9912Info));
           if(tw9912_info.this_is_first_open == true)
           {
                   ALOGE("Flyvideo-:第一次打开DVD,重新preview\n");
                   mHalCamCtrl->stopPreview();
                   //      sleep(1);
                   mHalCamCtrl->startPreview();
                   //      sleep(1);
           }
           tw9912_info.this_is_first_open = false;
           write(file_fd, (const void *)(&tw9912_info),sizeof(TW9912Info));
						OPEN_ERR:
           global_Fream_is_first = false;
   }

if(video_show_status[0] == '0' && rePreview_count > 100)
	{//发现黑屏 且 视频在上次打开没有视频源输入
		ALOGE("Flyvideo-上次视频打开，视频源无视频输入，stopPreview()-->\n");
		mHalCamCtrl->stopPreview();
		ALOGE("Flyvideo-上次视频打开，视频源无视频输入，startPreview()-->\n");
		mHalCamCtrl->startPreview();
		rePreview_count = 0;
	}
	else if(video_show_status[0] == '0')
	{
		rePreview_count++;
		if(rePreview_count >10000)rePreview_count = 101;
	}
  if (mHalCamCtrl->isNoDisplayMode()) {
    return processPreviewFrameWithOutDisplay(frame);
  } else {
				if(flymFps > FrameCount_flag)//if is DVD Not Discard
				{
					if(index ==0)
					{
						index =1;
						//if(pal_enabel[0] != '4')
						if(1)
						{//！PAL
							if(video_channel_status[0] != '3')
							{//倒车状态先判断黑屏再判断分屏
								if( DetermineImageSplitScreen(frame,mHalCamCtrl,flymFps,video_channel_status,flynow) )//发现分屏这个帧丢弃不显示
								return processPreviewFrameWithOutDisplay(frame);
							}
							else//Astren
							{
								if(BlackJudge(frame) == 1)//发现黑屏
									//return processPreviewFrameWithOutDisplay(frame);
									ALOGE("Flyvideo-：发现黑屏，但是目前还未作任何处理\n");
								else
									{
									if( DetermineImageSplitScreen(frame,mHalCamCtrl,flymFps,video_channel_status,flynow) )//发现分屏
									return processPreviewFrameWithOutDisplay(frame);
									else
									return processPreviewFrameWithDisplay(frame);
									}
							}
						return processPreviewFrameWithDisplay(frame);
						}
						else
							return processPreviewFrameWithDisplay(frame);
					}
					else//丢帧
					{
						index =0;
						return processPreviewFrameWithOutDisplay(frame);
					}
				}
				else//帧率低 全显
				{
					//if(pal_enabel[0] != '4')
					if(1)
					{//！PAL
						if(video_channel_status[0] != '3')
						{//倒车状态先判断黑屏再判断分屏
							if( DetermineImageSplitScreen(frame,mHalCamCtrl,flymFps,video_channel_status,flynow) )//发现分屏这个帧丢弃不显示
							return processPreviewFrameWithOutDisplay(frame);
						}
						else//Astren
						{
							if(BlackJudge(frame) == 1)//发现黑屏
								//return processPreviewFrameWithOutDisplay(frame);
								ALOGE("Flyvideo-发现出现黑屏，但是目前还未作任何处理\n");
							else
								{
								if( DetermineImageSplitScreen(frame,mHalCamCtrl,flymFps,video_channel_status,flynow) )//发现分屏
								return processPreviewFrameWithOutDisplay(frame);
								else
								return processPreviewFrameWithDisplay(frame);
								}
						}
					return processPreviewFrameWithDisplay(frame);
					}
					else
						return processPreviewFrameWithDisplay(frame);
				}
		}
}

// ---------------------------------------------------------------------------
// QCameraStream_preview
// ---------------------------------------------------------------------------

QCameraStream_preview::
QCameraStream_preview(int cameraId, camera_mode_t mode)
  : QCameraStream(cameraId,mode),
    mLastQueuedFrame(NULL),
    mNumFDRcvd(0)
  {
    mHalCamCtrl = NULL;
    ALOGE("%s: E", __func__);
    ALOGE("%s: X", __func__);
  }
// ---------------------------------------------------------------------------
// QCameraStream_preview
// ---------------------------------------------------------------------------

QCameraStream_preview::~QCameraStream_preview() {
    ALOGI("%s: E", __func__);
	if(mActive) {
		stop();
	}
	if(mInit) {
		release();
	}
	mInit = false;
	mActive = false;
    ALOGI("%s: X", __func__);

}
// ---------------------------------------------------------------------------
// QCameraStream_preview
// ---------------------------------------------------------------------------

status_t QCameraStream_preview::init() {

  status_t ret = NO_ERROR;
  ALOGI("%s: E", __func__);

  ret = QCameraStream::initChannel (mCameraId, MM_CAMERA_CH_PREVIEW_MASK);
  if (NO_ERROR!=ret) {
    ALOGE("%s E: can't init native cammera preview ch\n",__func__);
    return ret;
  }

  /* register a notify into the mmmm_camera_t object*/
  (void) cam_evt_register_buf_notify(mCameraId, MM_CAMERA_CH_PREVIEW,
                                     preview_notify_cb,
                                     MM_CAMERA_REG_BUF_CB_INFINITE,
                                     0,this);
  buffer_handle_t *buffer_handle = NULL;
  int tmp_stride = 0;
  mInit = true;
  return ret;
}
// ---------------------------------------------------------------------------
// QCameraStream_preview
// ---------------------------------------------------------------------------

status_t QCameraStream_preview::start()
{
    ALOGI("%s: E", __func__);
    status_t ret = NO_ERROR;
    cam_format_t previewFmt;

    Mutex::Autolock lock(mStopCallbackLock);
globao_mFlyPreviewStatus = 1;
ALOGE("Flyvideo-mFlyPreviewStatus =%d\n",globao_mFlyPreviewStatus);
    /* call start() in parent class to start the monitor thread*/
    //QCameraStream::start ();
    previewFmt = mHalCamCtrl->getPreviewFormat();
    setFormat(MM_CAMERA_CH_PREVIEW_MASK, previewFmt);

    if (mHalCamCtrl->isNoDisplayMode()) {
      if(NO_ERROR!=initPreviewOnlyBuffers()){
         return BAD_VALUE;
      }
    } else {
      if(NO_ERROR!=initDisplayBuffers()){
        return BAD_VALUE;
      }
    }
    ALOGE(" %s : initDisplayBuffers",__func__);
    ret = cam_config_prepare_buf(mCameraId, &mDisplayBuf);
    if(ret != MM_CAMERA_OK) {
      ALOGV("%s:reg preview buf err=%d\n", __func__, ret);
      ret = BAD_VALUE;
      goto error;
    } else {
      ret = NO_ERROR;
    }

    /* For preview, the OP_MODE we set is dependent upon whether we are
       starting camera or camcorder. For snapshot, anyway we disable preview.
       However, for ZSL we need to set OP_MODE to OP_MODE_ZSL and not
       OP_MODE_VIDEO. We'll set that for now in CamCtrl. So in case of
       ZSL we skip setting Mode here */

    if (!(myMode & CAMERA_ZSL_MODE)) {
        ALOGE("Setting OP MODE to MM_CAMERA_OP_MODE_VIDEO");
        mm_camera_op_mode_type_t op_mode=MM_CAMERA_OP_MODE_VIDEO;
        ret = cam_config_set_parm (mCameraId, MM_CAMERA_PARM_OP_MODE,
                                        &op_mode);
        ALOGE("OP Mode Set");

        if(MM_CAMERA_OK != ret) {
          ALOGE("%s: X :set mode MM_CAMERA_OP_MODE_VIDEO err=%d\n", __func__, ret);
          ret = BAD_VALUE;
          goto error;
        }
    }else {
        ALOGE("Setting OP MODE to MM_CAMERA_OP_MODE_ZSL");
        mm_camera_op_mode_type_t op_mode=MM_CAMERA_OP_MODE_ZSL;
        ret = cam_config_set_parm (mCameraId, MM_CAMERA_PARM_OP_MODE,
                                        &op_mode);
        if(MM_CAMERA_OK != ret) {
          ALOGE("%s: X :set mode MM_CAMERA_OP_MODE_ZSL err=%d\n", __func__, ret);
          ret = BAD_VALUE;
          goto error;
        }
     }

    /* call mm_camera action start(...)  */

    ALOGE("Starting Preview/Video Stream. ");
    ret = cam_ops_action(mCameraId, TRUE, MM_CAMERA_OPS_PREVIEW, 0);
    if (MM_CAMERA_OK != ret) {
      ALOGE ("%s: preview streaming start err=%d\n", __func__, ret);
      ret = BAD_VALUE;
      goto error;
    }

    ret = NO_ERROR;

    mActive =  true;
    goto end;

error:

    if (!mHalCamCtrl->isNoDisplayMode())
      putBufferToSurface();
    else
      freeBufferNoDisplay();

end:
    ALOGE("%s: X", __func__);
    return ret;
  }


// ---------------------------------------------------------------------------
// QCameraStream_preview
// ---------------------------------------------------------------------------
  void QCameraStream_preview::stop() {
    ALOGE("%s: E", __func__);
    int ret=MM_CAMERA_OK;
rePreview_count = 0;
globao_mFlyPreviewStatus = 0;
global_Fream_is_first = true;
open_dev_fail_count =0;
ALOGE("Flyvideo-mFlyPreviewStatus =%d\n",globao_mFlyPreviewStatus);
    if(!mActive) {
      return;
    }

    mStopCallbackLock.lock();
    mPreviewFrameLock.lock();
    mActive =  false;
    mPreviewFrameLock.unlock();
    /* unregister the notify fn from the mmmm_camera_t object*/

    ALOGI("%s: Stop the thread \n", __func__);
    /* call stop() in parent class to stop the monitor thread*/

    ret = cam_ops_action(mCameraId, FALSE, MM_CAMERA_OPS_PREVIEW, 0);
    if(MM_CAMERA_OK != ret) {
      ALOGE ("%s: camera preview stop err=%d\n", __func__, ret);
    }
    ret = cam_config_unprepare_buf(mCameraId, MM_CAMERA_CH_PREVIEW);
    if(ret != MM_CAMERA_OK) {
      ALOGE("%s:Unreg preview buf err=%d\n", __func__, ret);
    }

    /* In case of a clean stop, we need to clean all buffers*/
    /*free camera_memory handles and return buffer back to surface*/

#ifdef USE_ION
	if (vt_camera_enable)
		{
			close(ion_fddata.fd);
			struct ion_handle_data ion_handle2;
			ion_handle2.handle = ion_fddata.handle;
			if (ioctl(ion_rotation_fd, ION_IOC_FREE, &ion_handle2) < 0) ALOGE("CSVT: ion free failed\n");
			else ALOGE("CSVT: ion free success\n");
			close(ion_rotation_fd);
			close(fb_fd);
		}
#endif

    if (! mHalCamCtrl->isNoDisplayMode()) {
      putBufferToSurface();
    } else {
      freeBufferNoDisplay( );
    }


    ALOGE("%s: X", __func__);
    mStopCallbackLock.unlock();
  }
// ---------------------------------------------------------------------------
// QCameraStream_preview
// ---------------------------------------------------------------------------
  void QCameraStream_preview::release() {

    ALOGE("%s : BEGIN",__func__);
    int ret=MM_CAMERA_OK,i;

    if(!mInit)
    {
      ALOGE("%s : Stream not Initalized",__func__);
      return;
    }

    if(mActive) {
      this->stop();
    }

    ret= QCameraStream::deinitChannel(mCameraId, MM_CAMERA_CH_PREVIEW);
    ALOGE(": %s : De init Channel",__func__);
    if(ret != MM_CAMERA_OK) {
      ALOGE("%s:Deinit preview channel failed=%d\n", __func__, ret);
      //ret = BAD_VALUE;
    }

    (void)cam_evt_register_buf_notify(mCameraId, MM_CAMERA_CH_PREVIEW,
                                      NULL,
                                      (mm_camera_register_buf_cb_type_t)NULL,
                                      NULL,
                                      NULL);

    mInit = false;
    ALOGE("%s: END", __func__);

  }

QCameraStream*
QCameraStream_preview::createInstance(int cameraId,
                                      camera_mode_t mode)
{
  QCameraStream* pme = new QCameraStream_preview(cameraId, mode);
  return pme;
}
// ---------------------------------------------------------------------------
// QCameraStream_preview
// ---------------------------------------------------------------------------

void QCameraStream_preview::deleteInstance(QCameraStream *p)
{
  if (p){
    ALOGV("%s: BEGIN", __func__);
    p->release();
    delete p;
    p = NULL;
    ALOGV("%s: END", __func__);
  }
}

/* Temp helper function */
void *QCameraStream_preview::getLastQueuedFrame(void)
{
    return mLastQueuedFrame;
}

// ---------------------------------------------------------------------------
// No code beyone this line
// ---------------------------------------------------------------------------
}; // namespace android
