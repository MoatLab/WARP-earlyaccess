#!/bin/bash

#modprobe vfio-pci

for dev in "$@"; do
        vendor=$(cat /sys/bus/pci/devices/${dev}/vendor)
        device=$(cat /sys/bus/pci/devices/${dev}/device)
        if [ -e /sys/bus/pci/devices/${dev}/driver ]; then
                echo "${dev}" > /sys/bus/pci/devices/${dev}/driver/unbind
        fi
done

