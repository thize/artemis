#pragma once

#include <QObject>
#include <QStringList>

class MicrophoneDevices : public QObject
{
    Q_OBJECT

public:
    MicrophoneDevices();

    Q_PROPERTY(QStringList captureDevices MEMBER captureDevices NOTIFY captureDevicesChanged)

    Q_INVOKABLE void refresh();

signals:
    void captureDevicesChanged();

private:
    QStringList captureDevices;
};
