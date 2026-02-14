# NESPi w/Metro 2350 - DMG EMU

Parts needed:

* Adafruit Metro RP2350  
https://www.adafruit.com/product/6003


* Adafruit RP2350 22-pin FPC HSTX to DVI Adapter for HDMI Displays  
https://www.adafruit.com/product/6055


* STEMMA QT / Qwiic JST SH 4-Pin Cable - 200mm Long  
https://www.adafruit.com/product/4401


* 22-pin 0.5mm pitch FPC Flex Cable for DSI CSI or HSTX - 5cm  
https://www.adafruit.com/product/6034  
or  
* 22-pin 0.5mm pitch FPC Flex Cable for DSI CSI or HSTX - 10cm*  
https://www.adafruit.com/product/6035  


* I2C Socket & Breakout  
See included Gerber to make PCB or find similar on ebay, etc.  
You could alternatively cut an NES Classic extension cable


* NESPi Case  
* 3D Printed parts  
* VHB Tape  

* Controller:  
  * MyArcade Wireless Gamepad for NES Classic
  * NES Classic gamepad
  * Wii Gamepad

First, decide if you want to use a wireless receiver inside the case or mount an external jack for the controller.  
**Option 1**:  Internal wireless  
Doing this allows you to do the whole thing with zero case mods.  You'll leave the USB ports installed, just to block the holes.  This option is much easier.  
**Option 2**:  External jack  
This option requires a slight case mod and 3D printed parts.  You'll remove the USB ports and cut the vertical separator on the case where the ports were.


**Instructions**:  
Start by removing the extra components from the NESPi case.  You'll need to leave the PCB with the Power & Reset buttons, as well as the Micro USB (power) PCB.  If you decided to use the wireless gamepad receiver internally, you will need to leave the 2-USB port board.  Save all the screws as you'll use them for mounting to 3D printed parts.

Remove the screws for the Micro USB PCB, then reuse them to mount the 3D printed part for the HDMI PCB.  Then mount the HDMI PCB to the 3D printed part.

Attach the Metro RP2350 to the 3D printed mount using extra screws.

Install VHB tape to the 3D printed mount and stick in place.  Make sure to position carefully so that the case can still be assembled later.

Attach the flex cable to the HDMI board and the Metro RP2350, pins face downward.

Solder wires to the Wii/NES Classic I2C jack.  You can use a STEMMA QT / Qwiic JST SH or if you prefer, just use jumper wires to plug into the I2C connections on the Metro's pin header.

Make the connection from the I2C jack to the Metro.  Insert the JST-SH cable into the connector or use jumper wires.

Load the .uf2 file to the Metro RP2350

Assemble the case and test it out!


# Reference images

**Option 1: Internal Wireless:**  
![metro rp2350 wireless](./images/instructions/option1_internal_wireless.jpg?raw=true)  

**Option 2: External Controller Jack:**  
![metro rp2350 wired](./images/instructions/option2_external.jpg?raw=true)  


**Assembled Front:**  
![Assembled Front](./images/instructions/assembled_front.jpg?raw=true)  


![Assembled Front 2](./images/instructions/assembled_front2.jpg?raw=true)  

**Assembled Rear:**  
![Assembled Rear](./images/instructions/assembled_rear.jpg?raw=true)  

**Case Mod Before:**  
![Case Mod Before](./images/instructions/case_mod_before.jpg?raw=true)  

**Case Mod After:**  
![Case Mod After](./images/instructions/case_mod_after.jpg?raw=true)  

**Metro Mount:**  
![Metro Mount](./images/instructions/metro_mount.jpg?raw=true)  

**VHB Prep:**  
![VHB Prep](./images/instructions/vhb_prep.jpg?raw=true)  

**Wii/Classic NES Jack:**  
![Wii Jack](./images/instructions/wii_jack.jpg?raw=true)  

**Wii/Classic NES Jack Mount:**  
Note:  you might not even need screws on this mount  
(mechanical retention + friction fit + VHB tape)  
![Wii Jack Mount](./images/instructions/wii_jack_mount.jpg?raw=true)  

![Wii Jack Mount VHB](./images/instructions/wii_mount_vhb.jpg?raw=true)  

![Wii Jack Mount inst1](./images/instructions/wii_mount_inst1.jpg?raw=true)  

![Wii Jack Mount inst2](./images/instructions/wii_mount_inst2.jpg?raw=true)  
