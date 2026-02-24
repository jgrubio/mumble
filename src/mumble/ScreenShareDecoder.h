// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#ifndef MUMBLE_MUMBLE_SCREENSHAREDECODER_H_
#define MUMBLE_MUMBLE_SCREENSHAREDECODER_H_

#include <QImage>
#include <QObject>

#include <vpx/vpx_decoder.h>
#include <vpx/vp8dx.h>

class ScreenShareDecoder : public QObject {
	Q_OBJECT
public:
	explicit ScreenShareDecoder(QObject *parent = nullptr);
	~ScreenShareDecoder();

	bool init();
	bool isInitialized() const;

public slots:
	void decodeFrame(const QByteArray &encodedData, bool isKeyframe);

signals:
	void frameDecoded(QImage frame);

private:
	vpx_codec_ctx_t m_codec;
	bool m_initialized = false;
};

#endif // MUMBLE_MUMBLE_SCREENSHAREDECODER_H_
