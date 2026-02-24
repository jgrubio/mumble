// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#ifndef MUMBLE_MUMBLE_SCREENSHAREENCODER_H_
#define MUMBLE_MUMBLE_SCREENSHAREENCODER_H_

#include <QByteArray>
#include <QObject>

#include <vpx/vpx_encoder.h>
#include <vpx/vp8cx.h>

#include <cstdint>
#include <vector>

class ScreenShareEncoder : public QObject {
	Q_OBJECT
public:
	explicit ScreenShareEncoder(int width = 1280, int height = 720, int fps = 15, int bitrate = 1500,
								QObject *parent = nullptr);
	~ScreenShareEncoder();

	bool init();
	bool isInitialized() const;

public slots:
	void encodeFrame(const QByteArray &rgbFrame, int width, int height);

signals:
	void frameEncoded(QByteArray encodedData, bool isKeyframe, uint64_t frameNumber);

private:
	void convertRGBtoI420(const uint8_t *rgb, int srcWidth, int srcHeight, std::vector< uint8_t > &yuv);

	int m_width;
	int m_height;
	int m_fps;
	int m_bitrate;

	vpx_codec_ctx_t m_codec;
	vpx_codec_enc_cfg_t m_cfg;
	vpx_image_t m_rawFrame;
	bool m_initialized = false;

	uint64_t m_frameNumber = 0;
	std::vector< uint8_t > m_yuvBuffer;
};

#endif // MUMBLE_MUMBLE_SCREENSHAREENCODER_H_
