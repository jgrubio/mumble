// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#include "ScreenShareEncoder.h"

#include <QDebug>

#include <cstring>

ScreenShareEncoder::ScreenShareEncoder(int width, int height, int fps, int bitrate, QObject *parent)
	: QObject(parent), m_width(width), m_height(height), m_fps(fps), m_bitrate(bitrate) {
	std::memset(&m_codec, 0, sizeof(m_codec));
	std::memset(&m_cfg, 0, sizeof(m_cfg));
	std::memset(&m_rawFrame, 0, sizeof(m_rawFrame));
}

ScreenShareEncoder::~ScreenShareEncoder() {
	if (m_initialized) {
		vpx_codec_destroy(&m_codec);
		vpx_img_free(&m_rawFrame);
	}
}

bool ScreenShareEncoder::init() {
	if (m_initialized) {
		return true;
	}

	vpx_codec_err_t res = vpx_codec_enc_config_default(vpx_codec_vp8_cx(), &m_cfg, 0);
	if (res != VPX_CODEC_OK) {
		qWarning("ScreenShareEncoder: Failed to get default VP8 config: %s", vpx_codec_err_to_string(res));
		return false;
	}

	m_cfg.g_w                = static_cast< unsigned int >(m_width);
	m_cfg.g_h                = static_cast< unsigned int >(m_height);
	m_cfg.g_timebase.num     = 1;
	m_cfg.g_timebase.den     = m_fps;
	m_cfg.rc_target_bitrate  = static_cast< unsigned int >(m_bitrate);
	m_cfg.g_error_resilient  = VPX_ERROR_RESILIENT_DEFAULT;
	m_cfg.g_threads          = 2;
	m_cfg.kf_max_dist        = static_cast< unsigned int >(m_fps * 2);
	m_cfg.g_lag_in_frames    = 0;
	m_cfg.rc_end_usage       = VPX_CBR;
	m_cfg.g_pass             = VPX_RC_ONE_PASS;

	res = vpx_codec_enc_init(&m_codec, vpx_codec_vp8_cx(), &m_cfg, 0);
	if (res != VPX_CODEC_OK) {
		qWarning("ScreenShareEncoder: Failed to init VP8 encoder: %s", vpx_codec_err_to_string(res));
		return false;
	}

	vpx_codec_control(&m_codec, VP8E_SET_CPUUSED, 8);
	vpx_codec_control(&m_codec, VP8E_SET_STATIC_THRESHOLD, 800);

	if (!vpx_img_alloc(&m_rawFrame, VPX_IMG_FMT_I420, static_cast< unsigned int >(m_width),
					   static_cast< unsigned int >(m_height), 1)) {
		qWarning("ScreenShareEncoder: Failed to allocate VPX image");
		vpx_codec_destroy(&m_codec);
		return false;
	}

	m_yuvBuffer.resize(static_cast< size_t >(m_width * m_height * 3 / 2));
	m_initialized = true;
	return true;
}

bool ScreenShareEncoder::isInitialized() const {
	return m_initialized;
}

void ScreenShareEncoder::convertRGBtoI420(const uint8_t *rgb, int srcWidth, int srcHeight,
										  std::vector< uint8_t > &yuv) {
	const int ySize  = srcWidth * srcHeight;
	const int uvSize = (srcWidth / 2) * (srcHeight / 2);

	uint8_t *yPlane = yuv.data();
	uint8_t *uPlane = yuv.data() + ySize;
	uint8_t *vPlane = yuv.data() + ySize + uvSize;

	for (int j = 0; j < srcHeight; ++j) {
		for (int i = 0; i < srcWidth; ++i) {
			const int idx = (j * srcWidth + i) * 3;
			const int r   = rgb[idx];
			const int g   = rgb[idx + 1];
			const int b   = rgb[idx + 2];

			const int y = ((66 * r + 129 * g + 25 * b + 128) >> 8) + 16;
			yPlane[j * srcWidth + i] = static_cast< uint8_t >(y < 0 ? 0 : (y > 255 ? 255 : y));

			if ((j % 2 == 0) && (i % 2 == 0)) {
				const int uvIdx = (j / 2) * (srcWidth / 2) + (i / 2);
				const int u     = ((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128;
				const int v     = ((112 * r - 94 * g - 18 * b + 128) >> 8) + 128;
				uPlane[uvIdx]   = static_cast< uint8_t >(u < 0 ? 0 : (u > 255 ? 255 : u));
				vPlane[uvIdx]   = static_cast< uint8_t >(v < 0 ? 0 : (v > 255 ? 255 : v));
			}
		}
	}
}

void ScreenShareEncoder::encodeFrame(const QByteArray &rgbFrame, int width, int height) {
	if (!m_initialized) {
		return;
	}

	if (width != m_width || height != m_height) {
		qWarning("ScreenShareEncoder: Frame size mismatch: expected %dx%d, got %dx%d", m_width, m_height, width,
				 height);
		return;
	}

	convertRGBtoI420(reinterpret_cast< const uint8_t * >(rgbFrame.constData()), width, height, m_yuvBuffer);

	m_rawFrame.planes[VPX_PLANE_Y] = m_yuvBuffer.data();
	m_rawFrame.planes[VPX_PLANE_U] = m_yuvBuffer.data() + (width * height);
	m_rawFrame.planes[VPX_PLANE_V] = m_yuvBuffer.data() + (width * height) + (width / 2) * (height / 2);
	m_rawFrame.stride[VPX_PLANE_Y] = width;
	m_rawFrame.stride[VPX_PLANE_U] = width / 2;
	m_rawFrame.stride[VPX_PLANE_V] = width / 2;

	vpx_codec_err_t res =
		vpx_codec_encode(&m_codec, &m_rawFrame, static_cast< vpx_codec_pts_t >(m_frameNumber), 1, 0, VPX_DL_REALTIME);
	if (res != VPX_CODEC_OK) {
		qWarning("ScreenShareEncoder: Encode failed: %s", vpx_codec_err_to_string(res));
		return;
	}

	vpx_codec_iter_t iter = nullptr;
	const vpx_codec_cx_pkt_t *pkt;
	while ((pkt = vpx_codec_get_cx_data(&m_codec, &iter)) != nullptr) {
		if (pkt->kind == VPX_CODEC_CX_FRAME_PKT) {
			QByteArray encoded(reinterpret_cast< const char * >(pkt->data.frame.buf),
							   static_cast< int >(pkt->data.frame.sz));
			bool isKeyframe = (pkt->data.frame.flags & VPX_FRAME_IS_KEY) != 0;
			emit frameEncoded(encoded, isKeyframe, m_frameNumber);
		}
	}

	++m_frameNumber;
}
