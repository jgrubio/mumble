// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#ifndef MUMBLE_MUMBLE_SCREENSHAREVIEWER_H_
#define MUMBLE_MUMBLE_SCREENSHAREVIEWER_H_

#include <QImage>
#include <QLabel>
#include <QWidget>

/// Window that displays a remote user's screen share stream.
class ScreenShareViewer : public QWidget {
	Q_OBJECT
public:
	explicit ScreenShareViewer(const QString &userName, QWidget *parent = nullptr);
	~ScreenShareViewer() override;

public slots:
	void updateFrame(QImage frame);

protected:
	void resizeEvent(QResizeEvent *event) override;
	void closeEvent(QCloseEvent *event) override;

signals:
	void viewerClosed();

private:
	void updatePixmap();

	QLabel *m_label;
	QImage m_currentFrame;
};

#endif // MUMBLE_MUMBLE_SCREENSHAREVIEWER_H_
