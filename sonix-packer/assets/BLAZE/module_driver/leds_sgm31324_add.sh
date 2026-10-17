insmod leds_sgm31324_add.ko
cd /sys/module/leds_sgm31324_add/parameters/
echo i2c_bus_num=0 > sgm31324
echo enable_gpio=-1 > sgm31324
echo enable_active_level=-1 > sgm31324
echo wled_gpio=PC01 > sgm31324
echo wled_active_level=1 > sgm31324
echo wled_pattern_id=11 > sgm31324
echo alloc=21 > sgm31324
echo regs="00000000450007010106"  > sgm31324
echo regs="000000005400000B3800"  > sgm31324
echo regs="00000000450006060000"  > sgm31324
echo regs="00000000540000044000"  > sgm31324
echo regs="00000000000000000006"  > sgm31324
echo regs="001E7D0060BB00002F06"  > sgm31324
echo regs="000E7D00606600003F06"  > sgm31324
echo regs="00000000440000050006"  > sgm31324
echo regs="0000000045000D000006"  > sgm31324
echo regs="00000000510005001C06"  > sgm31324
echo regs="00000000000000000006"  > sgm31324
# Patterns 12-14, which the Blaze stock leaves out: Wi-Fi transfer, podcast, Bluetooth receiver.
# 13 is the R3 Pro II value with the red and blue bytes swapped; 12 and 14 were tuned on the device
# (14 is the stock blue, pattern 9, at full brightness).
echo regs="000E7D00686601070106"  > sgm31324
echo regs="00000000510007001206"  > sgm31324
echo regs="0000000045001F000006"  > sgm31324
# Pattern 15: solid red for the LED page, the red channel on its own at the current of pattern 6.
echo regs="00000000500000002F06"  > sgm31324
# Pattern 16: breathing blue, the stock blink of pattern 7 moved to the blue channel at the current of 14.
echo regs="000E7D0042661F000006"  > sgm31324
# Patterns 17-21: the same breathing on the other colours, each with the currents of its solid
# pattern (1 light blue, 3 cyan, 2 yellow, 4 orange, 13 magenta) and every lit channel on PWM1.
echo regs="000E7D004A6607010106"  > sgm31324
echo regs="000E7D004A6606060006"  > sgm31324
echo regs="000E7D006866000B3806"  > sgm31324
echo regs="000E7D00686600044006"  > sgm31324
echo regs="000E7D00626607001206"  > sgm31324
echo register > sgm31324
