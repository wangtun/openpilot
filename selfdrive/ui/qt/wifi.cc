#include "qt/wifi.hpp"

#include <QDebug>
#include <QListWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPixmap>
#include <QPushButton>

typedef QMap<QString, QMap<QString, QVariant> > Connection;
Q_DECLARE_METATYPE(Connection)

QString nm_path = "/org/freedesktop/NetworkManager";
QString nm_settings_path = "/org/freedesktop/NetworkManager/Settings";

QString nm_iface = "org.freedesktop.NetworkManager";
QString props_iface = "org.freedesktop.DBus.Properties";
QString nm_settings_iface = "org.freedesktop.NetworkManager.Settings";
QString device_iface = "org.freedesktop.NetworkManager.Device";
QString wireless_device_iface = "org.freedesktop.NetworkManager.Device.Wireless";
QString ap_iface = "org.freedesktop.NetworkManager.AccessPoint";

QString nm_service = "org.freedesktop.NetworkManager";



template <typename T>
T get_response(QDBusMessage response){
  QVariant first =  response.arguments().at(0);
  QDBusVariant dbvFirst = first.value<QDBusVariant>();
  QVariant vFirst = dbvFirst.variant();
  return vFirst.value<T>();
}


WifiSettings::WifiSettings(QWidget *parent) : QWidget(parent) {
  qDBusRegisterMetaType<Connection>();
  QString adapter = get_adapter();
  request_scan(adapter);
  QList<Network> networks = get_networks(adapter);

  QVBoxLayout *vlayout = new QVBoxLayout;

  for (auto &network : networks){
    unsigned int strength = std::round(network.strength / 25.0) * 25;

    qDebug() << network.ssid << network.strength << strength;
    QHBoxLayout *hlayout = new QHBoxLayout;
    hlayout->addWidget(new QLabel(QString::fromUtf8(network.ssid)));
    QPixmap pix("../assets/offroad/indicator_wifi_" + QString::number(strength) + ".png");
    QLabel *icon = new QLabel();
    icon->setPixmap(pix.scaledToWidth(100, Qt::SmoothTransformation));
    icon->setSizePolicy(QSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed));
    hlayout->addWidget(icon);
    hlayout->addSpacing(20);

    QPushButton * button = new QPushButton("Connect");
    button->setFixedWidth(200);
    hlayout->addWidget(button);
    hlayout->addSpacing(20);
    vlayout->addLayout(hlayout);
  }

  setLayout(vlayout);


  setStyleSheet(R"(
    QLabel { font-size: 40px }
    QPushButton { font-size: 40px }
    * {
      background-color: #114265;
    }
  )");

  // TODO: Handle NetworkManager not running
  // TODO: Handle no wireless adapter found
  // TODO: periodically request scan
  // TODO: periodically update network list
  // TODO: read currently connected SSID and show connected in list
  // TODO: implement connecting (including case with wrong password)
}

QList<Network> WifiSettings::get_networks(QString adapter){
  QList<Network> r;
  QDBusConnection bus = QDBusConnection::systemBus();
  QDBusInterface nm(nm_service, adapter, wireless_device_iface, bus);
  QDBusMessage response = nm.call("GetAllAccessPoints");
  QVariant first =  response.arguments().at(0);

  const QDBusArgument &args = first.value<QDBusArgument>();
  args.beginArray();
  while (!args.atEnd()) {
    QDBusObjectPath path;
    args >> path;

    // TODO: also return security type
    QDBusInterface device_props(nm_service, path.path(), props_iface, bus);
    QDBusMessage response = device_props.call("Get", ap_iface, "Ssid");
    QByteArray ssid = get_response<QByteArray>(response);

    response = device_props.call("Get", ap_iface, "Strength");
    unsigned int strength = get_response<unsigned int>(response);
    Network network = {ssid, strength};

    if (ssid.length()){
      r.push_back(network);
    }
  }
  args.endArray();

  return r;
}

void WifiSettings::connect(QByteArray ssid, QString password){
  // TODO: handle different authentication types, None, WEP, WPA, WPA Enterprise
  // TODO: hande exisiting connection for same ssid

  Connection connection;
  connection["connection"]["type"] = "802-11-wireless";
  connection["connection"]["uuid"] = QUuid::createUuid().toString().remove('{').remove('}');
  connection["connection"]["id"] = "Connection 1";

  connection["802-11-wireless"]["ssid"] = ssid;
  connection["802-11-wireless"]["mode"] = "infrastructure";

  connection["802-11-wireless-security"]["key-mgmt"] = "wpa-psk";
  connection["802-11-wireless-security"]["auth-alg"] = "open";
  connection["802-11-wireless-security"]["psk"] = password;

  connection["ipv4"]["method"] = "auto";
  connection["ipv6"]["method"] = "ignore";


  QDBusConnection bus = QDBusConnection::systemBus();
  QDBusInterface nm_settings(nm_service, nm_settings_path, nm_settings_iface, bus);
  QDBusReply<QDBusObjectPath> result = nm_settings.call("AddConnection", QVariant::fromValue(connection));
  if (!result.isValid()) {
    qDebug() << result.error().name() << result.error().message();
  } else {
    qDebug() << result.value().path();
  }

}

void WifiSettings::request_scan(QString adapter){
  QDBusConnection bus = QDBusConnection::systemBus();
  QDBusInterface nm(nm_service, adapter, wireless_device_iface, bus);
  QDBusMessage response = nm.call("RequestScan",  QVariantMap());

  qDebug() << response;
}

QString WifiSettings::get_adapter(){
  QDBusConnection bus = QDBusConnection::systemBus();

  QDBusInterface nm(nm_service, nm_path, nm_iface, bus);
  QDBusMessage response = nm.call("GetDevices");
  QVariant first =  response.arguments().at(0);

  QString adapter_path = "";

  const QDBusArgument &args = first.value<QDBusArgument>();
  args.beginArray();
  while (!args.atEnd()) {
    QDBusObjectPath path;
    args >> path;

    // Get device type
    QDBusInterface device_props(nm_service, path.path(), props_iface, bus);
    QDBusMessage response = device_props.call("Get", device_iface, "DeviceType");
    uint device_type = get_response<uint>(response);

    if (device_type == 2){ // Wireless
      adapter_path = path.path();
      break;
    }
  }
  args.endArray();

  return adapter_path;
}
