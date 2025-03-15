import pydbus
BLUEZ_SERVICE = 'org.bluez'
ADAPTER_PATH = '/org/bluez/hci0'
bus = pydbus.SystemBus()
adapter = bus.get(BLUEZ_SERVICE, ADAPTER_PATH) 
print(bus)
print(adapter)
