# emupod
Apple iPod emulator for Becker Indianapolis car head unit. Emupod communicates with the
Becker unit using the AAP (Apple Application Protocol). You do NOT need the Becker iPod adapter.

The emupod program can be run on a Ledato NanosG20 Linux minicomputer, stored in the glove compartment.

You also need an USB audio adapter; connect the output to the Line input of the Becker head unit.
Connect the RS-232 port on the Becker (sorry, I forgot what they call it; it's where the remote is
connected) to the serial port on the NanosG20.

The power consumption is less than 1W, meaning that you can leave it powered on for weeks while the
car is parked, without draining the battery. You need a 5V adapter for power; the usual "cigar lighter" 
adapters make an unpleasent noise on the audio side; I used an 5V Automotive evaluation board from
Linear Technology, with 300 kHz switching frequency,

emupod plays MP3 files and is operated from the Multimedia menu on the Becker. Music files are to
be stored on the usual Artist/Album/Song hierarchy; root of the tree must current dir for emupod.

The playing itself is taken care of by a patched version of the mpg123 player by Michael Hipp.
The patch file for mpg123 version 1.13.3 is included here.

I wrote this program in 2011, installed it with the computer in my Mondeo 2.5 Estate, and used it
with much pleasure for 3 years, until the car was replaced.

/Henrik A. Jacobsen
