# Pimoroni Galactic Unicorn
- - -
*An aggregate summary document compiled by hand for reference purposes* 
- - -
### Gorgeous programmable LED matrix displays with wireless connectivity and oodles of fancy extras! 🦄[^1]
Our space Unicorns are very beautiful, **all-in-one, RP2350 powered LED matrices** perfect for conveying information fabulously (or for sprucing up your desktop, makingspace or imaginarium). There's a ton of features aboard, here are some of our favourites!
**🌈 Loads* of RGB LEDs**, all with individual colour and brightness control. We're in mad love with these big bright squircular LEDs with their rounded apertures and built in diffusion.
**📷 Looks great on video** - invoking RP2350 magic means we can update the LEDs really quickly (we measured around 300 fps at 14-bit precision). This means there's no nasty strobing, artifacting or brightness stepping when it's filmed, so it's perfect for adding to the background of your streaming setup.
🌍 **2.4GHz wireless connectivity** (courtesy of the Raspberry Pi Pico 2 W) so you can use it to display all sorts of interesting data from the internet.
There's also an **onboard amp and little speaker** for bleepy alerts and *futuristic noises* and a **battery connector** so you can power it without it having to be tethered to a USB port. Every Unicorn comes with a pair of sleek little metal legs so it can stand up on its own (and has a selection of mounting holes if you'd prefer to do something else).  
Use it to make a very fancy clock, a very fancy weather display or a very fancy output for sensors (other very fancy use cases are available).
### What's new? 😎
As of mid-December 2024, Pico Unicorns are now **Pico 2 W Aboard**! Pico 2 W and it's souped-up RP2350 chip bring some exciting improvements - including a **higher core clock speed**, **double the on-chip SRAM**and **double the on-board flash memory**. RP2350 adds hardware support for floating point number crunching, which means graphical effects and demos should run even faster!
### Unicorn Physical Specs
|  **Unicorn** | ***How many LEDs** | **Arranged in a...**  | **Size of board (L x W x D)** |
|--------------|--------------------|-----------------------|-------------------------------|
| **Galactic** | **583**            | **53 x 11 wide grid** | **330 x 78 x 10.2 mm**        |

### Features
Raspberry Pi Pico 2 W Aboard
Dual Arm Cortex M33 running at up to 150MHz with 520KB of SRAM
4MB of QSPI flash supporting XiP
Powered and programmable by USB micro-B
2.4GHz wireless
256/583/1024 RGB LEDs in a 16x16/53x11/32x32 grid
3.5mm LEDs with rounded square apertures
6mm LED spacing
Driven by FM6047 constant current LED drivers
MAX98357 3.2W I2S Mono Amplifier (with 30mm 1W speaker)
Phototransistor for light sensing
9 tactile user buttons
Reset button
2x Qw/ST (Qwiic/STEMMA QT) connectors
JST-PH connector for attaching a battery (5.5V max)
Fully assembled
No soldering required.
Programmable with ~[C/C++](https://github.com/pimoroni/pimoroni-pico)~ or ~[MicroPython](https://github.com/pimoroni/unicorn)~
Schematics - ~[Stellar](https://cdn.shopify.com/s/files/1/0174/1800/files/stellar_unicorn_schematic.pdf?v=1688395833)~ / ~[Galactic](https://cdn.shopify.com/s/files/1/0174/1800/files/galactic_unicorn_schematic.pdf?v=1667910096)~ / ~[Cosmic](https://cdn.shopify.com/s/files/1/0174/1800/files/cosmic_unicorn_schematic.pdf?v=1677236038)~
### Kit includes
Stellar/Galactic/Cosmic Unicorn (with speaker attached)
2 x metal legs
USB A to micro-B cable
### Getting Started
To make it easy to get started, all our Unicorns ship **pre-loaded with pirate brand MicroPython** and a demo reel of pretty examples to stare at.
You can find more examples on Github:
~[Unicorn MicroPython firmware and examples](https://github.com/pimoroni/unicorn)~
### Not your everyday RGB LEDs
In our software, we use the Pico 2 W's PIOs (Programmable IOs) to drive the LEDs. Internally, Unicorn applies gamma correction to the supplied image data and updates the display with 14-bit precision resulting in extremely linear visual output - including at the low end.  The display is refreshed around 300 times per second (300fps!) allowing for rock solid stability even when being filmed, no smearing or flickering even when in motion.
### Connecting Breakouts
The Qw/ST connectors make it super easy to connect up ~[Qwiic](https://shop.pimoroni.com/collections/qwiic)~ or ~[STEMMA QT](https://shop.pimoroni.com/collections/stemma-qt)~ breakouts. If your breakout has a QW/ST connector on board, you can plug it straight in with a ~[JST-SH to JST-SH cable](https://shop.pimoroni.com/products/jst-sh-cable-qwiic-stemma-qt-compatible)~. 
Breakout Garden breakouts that don't have a Qw/ST connector can be connected using a ~[JST-SH to JST-SH cable](https://shop.pimoroni.com/products/jst-sh-cable-qwiic-stemma-qt-compatible)~ plus a ~[Qw/ST to Breakout Garden adaptor](https://shop.pimoroni.com/products/stemma-qt-qwiic-to-breakout-garden-adapter)~. Want to use >2 breakouts at the same time? Try ~[this adaptor](https://shop.pimoroni.com/products/sparkfun-qwiic-multiport)~!
~[List of breakouts](https://github.com/pimoroni/pimoroni-pico)~ currently compatible with our C++/MicroPython build.
### Notes
Power consumption stats! ⚡ We measured Galactic and Cosmic Unicorn as consuming just over 1A at maximum brightness, full white. When choosing a battery, consider that the LEDs will look their absolute best when they have access to at least 3.6V of power. At lower voltage levels you will start to see the blue elements of the LEDs fading out - this starts to become very noticeable at 2.9V and below. For best results when running on battery, we'd suggest using a chunky LiPo (check out the extras for some suggestions).
Note that Unicorns have no battery charging hardware onboard -  this is so you can use either alkaline or LiPo batteries safely. You'll need to charge up your LiPo battery with a separate battery charger (we like ~[LiPo Amigo](https://shop.pimoroni.com/products/lipo-amigo)~).
Squircle alert! 🟪🔵 Some batches of Unicorns have LEDs with more rounded apertures (though the LED specs and brightness remain the same). They all look good, but if you want a perfectly visually matched pair (or stable) of Unicorns it's probably best to buy them at the same time.
### Printables
Want to cut a diffuser or 3D print a case? Check these out:
**Galactic**
Galactic's 8 mounting holes are M2, 3mm in from the edge, and equally spaced 108mm horizontally and 72mm vertically. The leg holes are M2.5 (we've added two sets so you can adjust the angle).
.dxf files for the board outline:
~[with corner mounting holes only](https://cdn.shopify.com/s/files/1/0174/1800/files/galactic_unicorn_without_holes.dxf?v=1668162600)~
~[with all holes](https://cdn.shopify.com/s/files/1/0174/1800/files/galactic_unicorn_with_holes.dxf?v=1668162600)~
~[Screen mount with pivot stand](https://www.printables.com/model/486269-galactic-unicorn-pico-w-aboard-screen-mount-pivot-)~
~[Templates for box joint case and diffuser grill](https://github.com/seanosteen/CheerClock)~
Don't have easy access to a laser cutter? We also sell diffusers ~[here.](https://shop.pimoroni.com/products/unicorn-diffuser-kit)~
### About RP2350
The RP2350 chip is the Double Quarter Pounder & Fries to the RP2040's Double Cheeseburger and can have one or more RISC-V burgers instead of either of the M33 ARMs, to stretch the metaphor.
In addition to the modern M33 ARM cores, there are sides of: more PIO capability, a variety of low power states for sipping electrons, a whole security system and some sprinklings of specialist digital video circuits to offload DVI/HDMI output.
You can expect a tasty boost in performance - our "real world" MicroPython tests are running up to 2x faster compared to RP2040, and floating point number crunching in C/C++ is up to 20x faster. The extra on-chip RAM will make a big difference when performing memory intensive operations (such as working with higher resolution displays) and even more can be added thanks to external PSRAM support.
RP2350 comes in two flavours - A (standard) and B (all the pins). The B chip has a stonking 48 usable GPIO pins, including 8 ADCs and 24 PWMs, and features on some of our new products. 
~[Click here to view all things RP2350!](https://shop.pimoroni.com/collections/rp2350)~

- - -
# Pimoroni Galactic Unicorn Hello World[^2]

## Contents

| 1 | Introduction | 2 | Connecting to other sensors | 2.1 | Connect to Arduino UNO | 3 | Programming | 3.1 | Thonny | 3.2 | Micropython | 4 | Text and Fonts | 4.1 | Font library | 5 | Sound | 6 | Effects | 7 |
Physically modeled fireIntroduction
583 RGB leds in 53x11 grid. Raspberry Pi Pico W (microcontroller), speaker with amplifier, two (2) I2C Stemma/qt sensor sockets (3 or 4 pin JST PH), a light sensor (phototransistor) facing front and nine (9) control buttons, a reset button, JST-PH battery connector.
## Connecting to other sensors
2 x QW/ST Connections (4 pin 1.00 mm pitch Stemma QT / Qwiic). Stemma QT works on both, 5V and 3.3V logic, but Pico W is a 3.3 V system and Arduino Uno 5V.
Stemma is Adafruit's [three or four pin JST PH with 2.00 mm pitch. The three pin for analog IO devices and four pin is for I2C.] Stemma QT is a smaller version of the four pin Stemma format, with a 1.0 mm pitch, and is only for I2C.
Qwiic is Sparkfun's connector type
The buttons and the QW/ST connectors are the only means to interface with Galactic Unicorn, and Wifi of course. It is possible to get StemmaQT to male jumper wire adapters for use with I2C devices or for basic GPIO access.
Stemma QT
Black for GND
Red for V+
Blue for SDA
Yellow for SCL
Galactic Unicorn uses GP4 and GP5 for its I2C interface. You can use the constants in the shared pimoroni module to set up the I2C interface. [https://github.com/pimoroni/pimoroni-pico/blob/main/micropython/modules/galactic_unicorn/README.md\#using-breakouts](https://github.com/pimoroni/pimoroni-pico/blob/main/micropython/modules/galactic_unicorn/README.md#using-breakouts)
Master, Slave
More information
[https://github.com/nickgammon/I2C_Anything](https://github.com/nickgammon/I2C_Anything)
[https://arduino.stackexchange.com/questions/16292/sending-and-receiving-different-types-of-data-via-i2c-in-arduino](https://arduino.stackexchange.com/questions/16292/sending-and-receiving-different-types-of-data-via-i2c-in-arduino)
## Connect to Arduino UNO
Pico W is a 3.3 V system and Uno 5V system, so a logic level shifter (converter) should be used. 
Pull-up resistors: I2C Bus Pullup Resistor Calculation (Texas Instruments, Feb 2015). 
**from** **amchine** **import** Pin, I2C
**from** **time** **import** sleep

MSG_SIZE=15
i2c = I2C(0, scl=PIN(17), sda=PIN(16), freq=100000)
addr = i2c.scan()[]
i2c.writeto(addr, 'Hi from Pi')
sleep(0.1)
a = i2c.readfrom(addr, MSG_SIZE)
print(a)
The Arduino Code is in Girhub: [https://github.com/tinkertechtrove/pico-pi-playing/blob/main/arduino-i2c/sketch_i2c_sub/sketch_i2c_sub.ino](https://github.com/tinkertechtrove/pico-pi-playing/blob/main/arduino-i2c/sketch_i2c_sub/sketch_i2c_sub.ino)

More information:
[https://www.youtube.com/watch?v=Wkk1aNWj6sQ](https://www.youtube.com/watch?v=Wkk1aNWj6sQ)
[https://www.youtube.com/watch?v=Wh-SjhngILU](https://www.youtube.com/watch?v=Wh-SjhngILU)
## Programming
Raspberry Pi Pico can be programmed using Micropython or C/C++. This will deal only with MicroPython.
To upload your file to Pico, it need to be put into bootloader mode: hold down the **bootsel** button while plugging the USB cable: it should show up as a drive called RPI-RP2.
IDE's
Thonny is the da facto
[VS Studio](https://dev.to/blues/your-first-steps-with-raspberry-pi-pico-and-visual-studio-code-4jbd): MicroPico (the id is paulober.pico-w-go). If not found use this [https://github.com/microsoft/vscode/issues/108147](https://github.com/microsoft/vscode/issues/108147) because some Linux distros use OpenVSX.
Picographics, see [https://github.com/pimoroni/pimoroni-pico/blob/main/micropython/modules/picographics/README.md](https://github.com/pimoroni/pimoroni-pico/blob/main/micropython/modules/picographics/README.md) There is functions for eg
drawing a line, circle, rectangle, triangle, polygon
limited sprite support
display jpeg files

## Thonny
Connect the USB cable while **bootsel** button is pressed: RPI-RP2 is found on device manager. If not working, check other cable.
Copy the *pimoroni-galactic_unicorn-v1.20.6-micropython.uf2* to RPI-RP2.
Start thonny; see the right down corner that correct device is connected. If not, choose it from the list (right down corner).
Program.
Run/ Stop / Load.
## Micropython
Some libraries are needed. Download from [https://github.com/pimoroni/pimoroni-pico/releases](https://github.com/pimoroni/pimoroni-pico/releases)
**from** **picographics** **import** PicoGraphics, DISPLAY_GALACTIC_UNICORN
Manual: [PicoGraphics](https://github.com/pimoroni/pimoroni-pico/tree/main/micropython/modules/picographics)
**from** **galactic** **import** GalacticUnicorn
Manual: [GalacticUnicorn](https://github.com/pimoroni/pimoroni-pico/blob/main/micropython/modules/galactic_unicorn/README.md)

**from** **machine** **import** Pin, I2C
## Text and Fonts
Stationary, centred, scrolling text.
## Font library
Nice 5x3 characters: [https://forums.pimoroni.com/t/galactic-unicorn-small-numeric-characters/20766](https://forums.pimoroni.com/t/galactic-unicorn-small-numeric-characters/20766)
## SoundEffects
Bouncing ball: [https://www.instructables.com/Galactic-Unicorn-Bounce-Simple-GFX-Demo/](https://www.instructables.com/Galactic-Unicorn-Bounce-Simple-GFX-Demo/)
## Physically modeled fire
Pressing B gives some flames, but this is not what I am looking for. It is located at GitHub [https://github.com/pimoroni/pimoroni-pico/blob/main/micropython/examples/galactic_unicorn/fire_effect.py](https://github.com/pimoroni/pimoroni-pico/blob/main/micropython/examples/galactic_unicorn/fire_effect.py)
See [https://www.youtube.com/watch?v=bRXrL-8CTmg](https://www.youtube.com/watch?v=bRXrL-8CTmg) for a simple idea of a flame.
Draw vertical sine wave and animate that
Draw a circle to the point where the flame originates. I'll use Bresenhams algorithm; see [https://www.geeksforgeeks.org/bresenhams-circle-drawing-algorithm/](https://www.geeksforgeeks.org/bresenhams-circle-drawing-algorithm/) However, there is no need to draw actual circle.
The circle should be expanded according to the sine function to look like a flame. Two easy option:
Use linear function; eg x in range(nmax+y/2-5) where y is the height (in pixels, inverted)
Use some probapilistic method; eg starting width and shorten it by one or 2.
Use smaller and larger circles with different colors. Colors
Yellow (Orange) smallest
Red mid-one
Yellow largest
Add more flames and make it asynchronous: make a flame object or sprite. Sprite would be easy, just create the animation in 128x128 pixel spritesheet. Thus, I'll try using objects.

 More detailed: [http://graphics.ucsd.edu/~henrik/papers/fire/fire.pdf](http://graphics.ucsd.edu/~henrik/papers/fire/fire.pdf)

Add flashing: [https://www.youtube.com/watch?v=T7LOWIrUKwY](https://www.youtube.com/watch?v=T7LOWIrUKwY)

- - -
[^1]: [Pimoroni Galactic Unicorn Pico 2W Product Page](https://shop.pimoroni.com/products/space-unicorns?variant=40842033561683)
[^2]: [Excerpts from Pimoroni Galactic Unicorn Hello World](https://wiki.luntti.net/index.php?title=Pimoroni_Galactic_Unicorn_Hello_World)
