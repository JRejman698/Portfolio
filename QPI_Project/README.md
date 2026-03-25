Folder contains main script - Wykresy_dla_komórki, wchich is responsible for running script komj in a loop, for every picture taken. 
To the script komj from the main script an argument is passed telling the script wchich picture to analize.
In the script we use the Detrend2D function to find a trend in an image and then substract it from the image to get better data.
Next step is to substract the background from the image. For the first image we choose the area that is the bacground and than chose our Region of Intrest (ROI), wchich is our cell.
The next steps of the loop will mesure values on the same cell
Additionally because our cell can change during the experiment and to get better aquracy in circulating the cell, we use a threshold algorithm to adjust ROI.
After that we calculate the parameters and write them to a file that can be read later.
