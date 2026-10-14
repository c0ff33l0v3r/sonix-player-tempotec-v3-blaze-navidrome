#!/bin/sh

ulimit -s 1024
export MALLOC_ARENA_MAX=1

killall    sonix_player    &>/dev/null
killall -9 sonix_player    &>/dev/null

if [ -f "/usr/bin/batd" ]; then
killall    batd    &>/dev/null
killall -9 batd    &>/dev/null
#/usr/bin/batd -v -s -t5 -o /mnt/sd_0/batlog.txt &
fi

# Lit by the cable and not by the power button (PMIC REG20h bit 2 is the cable,
# bit 0 the key; REG07h is the player's own byte, 1 once it has started): only
# the charging screen, the way the stock player stays off on its cable. The long
# press of the power button ends it and sonix_launch reboots into the player.
seen=$(i2cget -f -y 0 0x34 0x07 2>/dev/null)
src=$(i2cget -f -y 0 0x34 0x20 2>/dev/null)
if [ -x /usr/bin/sonix_charge ] && [ "$seen" = "0x00" ] && [ "$src" = "0x04" ]; then
# With ADB switched on in the settings it is up here too, to look at this screen.
grep -q '^adb = 1' /usr/data/device_config.ini 2>/dev/null && /etc/init.d/adb/S440adb start
[ -x /usr/bin/sonix_launch ] && exec /usr/bin/sonix_launch /usr/bin/sonix_charge
/usr/bin/sonix_charge
sleep 1
reboot
fi

#/usr/bin/sonix_player &>/dev/null
[ -x /usr/bin/sonix_launch ] && exec /usr/bin/sonix_launch /usr/bin/sonix_player
/usr/bin/sonix_player
sleep 1
reboot
