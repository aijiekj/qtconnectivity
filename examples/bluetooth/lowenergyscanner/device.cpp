/***************************************************************************
**
** Copyright (C) 2013 BlackBerry Limited. All rights reserved.
** Copyright (C) 2014 Digia Plc and/or its subsidiary(-ies).
** Contact: http://www.qt-project.org/legal
**
** This file is part of the QtBluetooth module of the Qt Toolkit.
**
** $QT_BEGIN_LICENSE:BSD$
** You may use this file under the terms of the BSD license as follows:
**
** "Redistribution and use in source and binary forms, with or without
** modification, are permitted provided that the following conditions are
** met:
**   * Redistributions of source code must retain the above copyright
**     notice, this list of conditions and the following disclaimer.
**   * Redistributions in binary form must reproduce the above copyright
**     notice, this list of conditions and the following disclaimer in
**     the documentation and/or other materials provided with the
**     distribution.
**   * Neither the name of Digia Plc and its Subsidiary(-ies) nor the names
**     of its contributors may be used to endorse or promote products derived
**     from this software without specific prior written permission.
**
**
** THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
** "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
** LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
** A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
** OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
** SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
** LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
** DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
** THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
** (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
** OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE."
**
** $QT_END_LICENSE$
**
****************************************************************************/

#include "device.h"
#include <qbluetoothaddress.h>
#include <qbluetoothdevicediscoveryagent.h>
#include <qbluetoothlocaldevice.h>
#include <qbluetoothdeviceinfo.h>
#include <qbluetoothservicediscoveryagent.h>
#include <QDebug>
#include <QList>

Device::Device():
    connected(false), controller(0), m_deviceScanState(false)
{
    discoveryAgent = new QBluetoothDeviceDiscoveryAgent();
    connect(discoveryAgent, SIGNAL(deviceDiscovered(const QBluetoothDeviceInfo&)),
            this, SLOT(addDevice(const QBluetoothDeviceInfo&)));
    connect(discoveryAgent, SIGNAL(error(QBluetoothDeviceDiscoveryAgent::Error)),
            this, SLOT(deviceScanError(QBluetoothDeviceDiscoveryAgent::Error)));
    connect(discoveryAgent, SIGNAL(finished()), this, SLOT(deviceScanFinished()));

    setUpdate("Search");
}

Device::~Device()
{
    delete discoveryAgent;
    delete controller;
    qDeleteAll(devices);
    qDeleteAll(m_services);
    qDeleteAll(m_characteristics);
    devices.clear();
    m_services.clear();
    m_characteristics.clear();
}

void Device::startDeviceDiscovery()
{
    qDeleteAll(devices);
    devices.clear();
    setUpdate("Scanning for devices ...");
    discoveryAgent->start();
    m_deviceScanState = true;
    Q_EMIT stateChanged();
}

void Device::addDevice(const QBluetoothDeviceInfo &info)
{
    if (info.coreConfigurations() & QBluetoothDeviceInfo::LowEnergyCoreConfiguration) {
        DeviceInfo *d = new DeviceInfo(info);
        devices.append(d);
        setUpdate("Last device added: " + d->getName());
    }
}

void Device::deviceScanFinished()
{
    emit devicesUpdated();
    m_deviceScanState = false;
    emit stateChanged();
    if (devices.isEmpty())
        setUpdate("No Low Energy devices found...");
    else
        setUpdate("Done! Scan Again!");
}

QVariant Device::getDevices()
{
    return QVariant::fromValue(devices);
}

QVariant Device::getServices()
{
    return QVariant::fromValue(m_services);
}

QVariant Device::getCharacteristics()
{
    return QVariant::fromValue(m_characteristics);
}

QString Device::getUpdate()
{
    return m_message;
}

void Device::scanServices(const QString &address)
{
    // We need the current device for service discovery.
    for (int i = 0; i < devices.size(); i++) {
        if (((DeviceInfo*)devices.at(i))->getAddress() == address )
            currentDevice.setDevice(((DeviceInfo*)devices.at(i))->getDevice());
    }

    if (!currentDevice.getDevice().isValid()) {
        qWarning() << "Not a valid device";
        return;
    }

    qDeleteAll(m_characteristics);
    m_characteristics.clear();
    qDeleteAll(m_services);
    m_services.clear();


    setUpdate("Connecting to device...");

    if (controller && controller->remoteAddress() != currentDevice.getDevice().address()) {
        controller->disconnectFromDevice();
        delete controller;
        controller = 0;
    }

    if (!controller) {
        // Connecting signals and slots for connecting to LE services.
        controller = new QLowEnergyControllerNew(currentDevice.getDevice().address());
        connect(controller, SIGNAL(connected()),
                this, SLOT(deviceConnected()));
        connect(controller, SIGNAL(error(QLowEnergyControllerNew::Error)),
                this, SLOT(errorReceived(QLowEnergyControllerNew::Error)));
        connect(controller, SIGNAL(disconnected()),
                this, SLOT(deviceDisconnected()));
        connect(controller, SIGNAL(serviceDiscovered(QBluetoothUuid)),
                this, SLOT(addLowEnergyService(QBluetoothUuid)));
        connect(controller, SIGNAL(discoveryFinished()),
                this, SLOT(serviceScanDone()));
    }

    controller->connectToDevice();
}

void Device::addLowEnergyService(const QBluetoothUuid &serviceUuid)
{
    QLowEnergyService *service = controller->createServiceObject(serviceUuid);
    if (!service) {
        qWarning() << "Cannot create service for uuid";
        return;
    }
    ServiceInfo *serv = new ServiceInfo(service);
    m_services.append(serv);

    emit servicesUpdated();
}

void Device::serviceScanDone()
{
    setUpdate("Service scan done!");
}

void Device::connectToService(const QString &uuid)
{
    QLowEnergyService *service = 0;
    for (int i = 0; i < m_services.size(); i++) {
        ServiceInfo *serviceInfo = (ServiceInfo*)m_services.at(i);
        if (serviceInfo->getUuid() == uuid) {
            service = serviceInfo->service();
            break;
        }
    }

    if (!service)
        return;

    qDeleteAll(m_characteristics);
    m_characteristics.clear();

    if (service->state() == QLowEnergyService::DiscoveryRequired) {
        connect(service, SIGNAL(stateChanged(QLowEnergyService::ServiceState)),
                this, SLOT(serviceDetailsDiscovered(QLowEnergyService::ServiceState)));
        service->discoverDetails();
        return;
    }

    //discovery already done
    const QList<QLowEnergyCharacteristic> chars = service->characteristics();
    foreach (const QLowEnergyCharacteristic &ch, chars) {
        CharacteristicInfo *cInfo = new CharacteristicInfo(ch);
        m_characteristics.append(cInfo);
    }

    emit characteristicsUpdated();
}

void Device::deviceConnected()
{

    setUpdate("Discovering services!");
    connected = true;
    controller->discoverServices();
}

void Device::errorReceived(QLowEnergyControllerNew::Error /*error*/)
{
    qWarning() << "Error: " << controller->errorString();
    setUpdate(controller->errorString());
}

void Device::setUpdate(QString message)
{
    m_message = message;
    emit updateChanged();
}

void Device::disconnectFromDevice()
{
    if (connected)
        controller->disconnectFromDevice();
}

void Device::deviceDisconnected()
{
    //setUpdate("Device disconnected " + currentDevice.getName());
}

void Device::serviceDetailsDiscovered(QLowEnergyService::ServiceState newState)
{
    if (newState != QLowEnergyService::ServiceDiscovered)
        return;

    QLowEnergyService *service = qobject_cast<QLowEnergyService *>(sender());
    if (!service)
        return;

    const QList<QLowEnergyCharacteristic> chars = service->characteristics();
    foreach (const QLowEnergyCharacteristic &ch, chars) {
        CharacteristicInfo *cInfo = new CharacteristicInfo(ch);
        m_characteristics.append(cInfo);
    }

    emit characteristicsUpdated();
}

void Device::deviceScanError(QBluetoothDeviceDiscoveryAgent::Error error)
{
    if (error == QBluetoothDeviceDiscoveryAgent::PoweredOffError)
        setUpdate("The Bluetooth adaptor is powered off, power it on before doing discovery.");
    else if (error == QBluetoothDeviceDiscoveryAgent::InputOutputError)
        setUpdate("Writing or reading from the device resulted in an error.");
    else
        setUpdate("An unknown error has occurred.");
}

bool Device::state()
{
    return m_deviceScanState;
}
