// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#ifndef MUMBLE_MUMBLE_SCREENSHARECAPTURE_H_
#define MUMBLE_MUMBLE_SCREENSHARECAPTURE_H_

#include <QByteArray>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QImage>
#include <QMutex>
#include <QObject>
#include <QTimer>

#include <cstdint>

#include <pipewire/stream.h>
#include <spa/utils/hook.h>

struct pw_context;
struct pw_core;
struct pw_loop;
struct pw_properties;
struct pw_thread_loop;

/// Captures the screen using XDG Desktop Portal + PipeWire.
/// The portal is used to get user consent and a PipeWire node ID,
/// then a PipeWire stream is connected to capture video frames.
class ScreenShareCapture : public QObject {
	Q_OBJECT
public:
	explicit ScreenShareCapture(int targetWidth = 1280, int targetHeight = 720, int fps = 15,
								QObject *parent = nullptr);
	~ScreenShareCapture();

	/// Start the XDG Desktop Portal flow to select a screen/window.
	void start();
	/// Stop capturing and clean up.
	void stop();

	bool isCapturing() const;

signals:
	void frameCaptured(QByteArray rgbFrame, int width, int height);
	void captureStarted();
	void captureStopped();
	void captureError(QString errorMessage);

private slots:
	void onCreateSessionResponse(const QDBusMessage &msg);
	void onSelectSourcesResponse(const QDBusMessage &msg);
	void onStartResponse(const QDBusMessage &msg);
	void onFrameTimer();

private:
	void createSession();
	void selectSources();
	void startPortal();
	bool setupPipeWireStream(uint32_t nodeId);
	void cleanupPipeWire();

	static void onStreamStateChanged(void *userdata, enum pw_stream_state old,
									 enum pw_stream_state state, const char *error);
	static void onStreamProcess(void *userdata);

	int m_targetWidth;
	int m_targetHeight;
	int m_fps;
	bool m_capturing = false;

	// XDG Desktop Portal
	QString m_sessionHandle;
	QDBusObjectPath m_sessionPath;
	QString m_senderName;
	uint m_requestCounter = 0;

	// PipeWire
	pw_loop *m_pwLoop            = nullptr;
	pw_context *m_pwContext       = nullptr;
	pw_core *m_pwCore             = nullptr;
	pw_stream *m_pwStream        = nullptr;
	pw_thread_loop *m_pwThread   = nullptr;
	pw_stream_events *m_pwEvents = nullptr;
	spa_hook m_pwStreamHook      = {};
	int m_pwFd                   = -1;

	// Frame timer for FPS control
	QTimer m_frameTimer;
	bool m_newFrameAvailable       = false;
	QByteArray m_latestFrame;
	int m_latestFrameWidth         = 0;
	int m_latestFrameHeight        = 0;
	QMutex m_frameMutex;
};

#endif // MUMBLE_MUMBLE_SCREENSHARECAPTURE_H_
