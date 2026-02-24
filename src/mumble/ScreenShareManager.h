// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#ifndef MUMBLE_MUMBLE_SCREENSHAREMANAGER_H_
#define MUMBLE_MUMBLE_SCREENSHAREMANAGER_H_

#include <QHash>
#include <QObject>

#include <cstdint>

class ClientUser;
class ScreenShareCapture;
class ScreenShareDecoder;
class ScreenShareEncoder;
class ScreenShareViewer;

/// Central orchestrator for screen sharing.
/// Manages local capture+encode for sending, and decode+display for receiving.
class ScreenShareManager : public QObject {
	Q_OBJECT
public:
	explicit ScreenShareManager(QObject *parent = nullptr);
	~ScreenShareManager();

	/// Start sharing the local screen.
	void startSharing();
	/// Stop sharing the local screen.
	void stopSharing();
	/// Whether we are currently sharing our screen.
	bool isSharing() const;

	/// Start viewing a remote user's screen share.
	void startViewing(ClientUser *user, uint32_t width, uint32_t height);
	/// Stop viewing a remote user's screen share.
	void stopViewing(ClientUser *user);
	/// Process a received video frame from a remote user.
	void receiveFrame(ClientUser *user, const QByteArray &frameData, bool isKeyframe, uint64_t frameNumber);

signals:
	void sharingStarted();
	void sharingStopped();

private slots:
	void onCaptureStarted();
	void onCaptureStopped();
	void onCaptureError(const QString &error);
	void onFrameCaptured(QByteArray rgbFrame, int width, int height);
	void onFrameEncoded(QByteArray encodedData, bool isKeyframe, uint64_t frameNumber);
	void onViewerClosed();

private:
	// Local sharing (sender)
	ScreenShareCapture *m_capture = nullptr;
	ScreenShareEncoder *m_encoder = nullptr;
	bool m_sharing                = false;

	// Remote viewing (receiver) — one decoder+viewer per remote user
	struct RemoteStream {
		ScreenShareDecoder *decoder = nullptr;
		ScreenShareViewer *viewer   = nullptr;
		uint32_t session            = 0;
	};
	QHash< uint32_t, RemoteStream > m_remoteStreams; // keyed by user session
};

#endif // MUMBLE_MUMBLE_SCREENSHAREMANAGER_H_
