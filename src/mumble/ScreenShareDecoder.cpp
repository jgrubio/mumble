// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#include "ScreenShareDecoder.h"

#include <QDebug>

#include <cstring>

ScreenShareDecoder::ScreenShareDecoder(QObject *parent) : QObject(parent) {
	std::memset(&m_codec, 0, sizeof(m_codec));
}

ScreenShareDecoder::~ScreenShareDecoder() {
	if (m_initialized) {
		vpx_codec_destroy(&m_codec);
	}
}

bool ScreenShareDecoder::init() {
	if (m_initialized) {
		return true;
	}

	vpx_codec_err_t res = vpx_codec_dec_init(&m_codec, vpx_codec_vp8_dx(), nullptr, 0);
	if (res != VPX_CODEC_OK) {
		qWarning("ScreenShareDecoder: Failed to init VP8 decoder: %s", vpx_codec_err_to_string(res));
		return false;
	}

	m_initialized = true;
	return true;
}

bool ScreenShareDecoder::isInitialized() const {
	return m_initialized;
}

void ScreenShareDecoder::decodeFrame(const QByteArray &encodedData, bool isKeyframe) {
	Q_UNUSED(isKeyframe);

	if (!m_initialized) {
		return;
	}

	vpx_codec_err_t res = vpx_codec_decode(&m_codec, reinterpret_cast< const uint8_t * >(encodedData.constData()),
											static_cast< unsigned int >(encodedData.size()), nullptr, 0);
	if (res != VPX_CODEC_OK) {
		qWarning("ScreenShareDecoder: Decode failed: %s", vpx_codec_err_to_string(res));
		return;
	}

	vpx_codec_iter_t iter = nullptr;
	vpx_image_t *img      = vpx_codec_get_frame(&m_codec, &iter);
	if (!img) {
		return;
	}

	const int w = static_cast< int >(img->d_w);
	const int h = static_cast< int >(img->d_h);

	QImage result(w, h, QImage::Format_RGB888);

	const uint8_t *yPlane = img->planes[VPX_PLANE_Y];
	const uint8_t *uPlane = img->planes[VPX_PLANE_U];
	const uint8_t *vPlane = img->planes[VPX_PLANE_V];
	const int yStride     = img->stride[VPX_PLANE_Y];
	const int uStride     = img->stride[VPX_PLANE_U];
	const int vStride     = img->stride[VPX_PLANE_V];

	for (int j = 0; j < h; ++j) {
		uint8_t *scanline = result.scanLine(j);
		for (int i = 0; i < w; ++i) {
			const int y = yPlane[j * yStride + i] - 16;
			const int u = uPlane[(j / 2) * uStride + (i / 2)] - 128;
			const int v = vPlane[(j / 2) * vStride + (i / 2)] - 128;

			int r = (298 * y + 409 * v + 128) >> 8;
			int g = (298 * y - 100 * u - 208 * v + 128) >> 8;
			int b = (298 * y + 516 * u + 128) >> 8;

			r = r < 0 ? 0 : (r > 255 ? 255 : r);
			g = g < 0 ? 0 : (g > 255 ? 255 : g);
			b = b < 0 ? 0 : (b > 255 ? 255 : b);

			scanline[i * 3]     = static_cast< uint8_t >(r);
			scanline[i * 3 + 1] = static_cast< uint8_t >(g);
			scanline[i * 3 + 2] = static_cast< uint8_t >(b);
		}
	}

	emit frameDecoded(result);
}
