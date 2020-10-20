#pragma once
#include "qt/keyboard.hpp"
#include <QWidget>
#include <QtDBus>
#include <QPushButton>
#include <QButtonGroup>

struct Network {
  QString path;
  QByteArray ssid;
  unsigned int strength;
};

class WifiSettings : public QWidget {
  Q_OBJECT

  private:
    QString get_adapter();
    QList<Network> get_networks(QString adapter);
    void connect_to(QByteArray ssid, QString password);
    void request_scan(QString adapter);
    QString get_active_ap(QString adapter);
    QByteArray get_ap_ssid(QString network_path);
    unsigned int get_ap_strength(QString network_path);
  public:
    explicit WifiSettings(QWidget *parent = 0);

  private slots:
    void handleButton(QAbstractButton* m_button);
};
