🔌 Power / Ground
From	To	Notes
Teensy VIN	5V Regulator Output	If powered from external 9V DC
Teensy GND	All pots, Audio Shield, jacks	Common ground rail
Teensy 3.3V	Potentiometer high sides (A0–A3)	Stable analog reference voltage
Teensy GND	Potentiometer low sides	Analog ground

🎛️ Potentiometers (10k Linear)
Function	Teensy Pin	Pot Connections
Loop Duration	A0	Wiper to A0, 3.3V to high, GND to low
Fade Time	A1	Wiper to A1, 3.3V to high, GND to low
Volume	A2	Wiper to A2, 3.3V to high, GND to low
Tone / EQ	A3	Wiper to A3, 3.3V to high, GND to low

👣 Footswitch (Momentary SPST)
From	To	Notes
One side	Teensy Pin 0	Pull-up enabled in code
Other side	GND	Standard pull-down behavior when pressed

📤 Audio Shield Connections (Teensy 4.1)
These are fixed when stacked or wired. If you're not stacking: use jumpers as follows.

Audio Shield Pin	Connects To Teensy Pin
MCLK	Pin 23
BCLK	Pin 21
LRCLK	Pin 20
DIN	Pin 7
DOUT	Pin 8
3.3V, GND	3.3V and GND

🎧 Audio I/O Jacks
Jack	Connects To	Notes
Input Jack Tip	Audio Shield Line In Left (via cap or resistor)	Use shielded cable
Input Jack Sleeve	GND	
Output Jack Tip	Audio Shield Line Out Left	Or headphone out if needed
Output Jack Sleeve	GND	

💡 LED (Optional)
From	To	Notes
Teensy digital pin (e.g. 13)	Resistor → LED anode	
LED cathode	GND	Use 220Ω resistor inline

🔌 Optional USB or DC Jack
Purpose	Connection
DC Jack (center +)	to 5V regulator input
USB breakout	to Teensy USB for programming access
