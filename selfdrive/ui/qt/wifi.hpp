#pragma once
#include <QWidget>
#include <QtDBus>

struct Network {
  QByteArray ssid;
  unsigned int strength;
};

class WifiSettings : public QWidget {
  Q_OBJECT

private:
  QString get_adapter();
  QList<Network> get_networks(QString adapter);
  void connect(QByteArray ssid, QString password);
  void request_scan(QString adapter);
public:
  explicit WifiSettings(QWidget *parent = 0);
};
