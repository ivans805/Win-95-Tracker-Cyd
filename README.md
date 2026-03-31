# Win-95-Tracker-Cyd
A little side Project i made:
I randomly saw a tracker on an website and thought Nothing of it, then Later i was wondering if i could play them on an Microcontroller and here i am today.
So this code requires i2s and runs on an 240x320 and an XPT2046 Setup.
This Tracker was mainly developed on An ESP32-2432S028(R) Board also known as "Cheap Yellow Display" and has all the pinouts for it, But a quick reminder for the people running the firmware an Such board:
If you want Clean perfectly times sound then you need to Install an i2s mod on Your cyd as i did.
# Cyd I2S Mod
You see the cyd only has 2 usable pins so you need to solder a wire to the led Or directly to the ESP32 as i Did it:
![WIN_20260331_22_14_49_Pro](https://github.com/user-attachments/assets/1cd0e3b1-f4ea-4903-a09d-b71b08dbeaaf)
(Sorry for the bad webcam quality)
# The UI
For the ui i started out as a basic prototype but wanted more from it. I went on Lopaka and Made the ui Into a windows 95 looking app.
The ui had a song progress bar But i removed it Because songs that reused Patterns Made the bar go crazy, And added a status text (inspired from Bassoontracker) that Also 
has a loading bar i also Took from bassoontracker. I also added squares around each individual effect in every channel that light up when a Effect/ Note Or sample gets hit in each channel.
<img width="682" height="518" alt="Screenshot 2026-03-27 193034" src="https://github.com/user-attachments/assets/c31f4e23-f886-4ad1-919c-416534406ddb" />
# Setup
Open The files In Arduino IDE and make sure you have all the libraries installed (Especially TFT_Espi)
Also set up tft espi when not set up.
Change the definements to your pinouts.
Make sure your device has the i2s driver (Modification needed if not)
Flash your device.
Put test files like false visage on your sd card.
Enjoy!
# Operation
https://github.com/user-attachments/assets/1b7a69d6-ee27-4c93-928b-97980feca906

