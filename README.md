SleepSmart


The SleepSmart pillow utilizes sleep variables like Heart rate variability and User sleep range preference to measure the user's stage 1 sleep in the specified range of wakeup time, ensuring you are energetic.
In this project we only use the max30102 heart rate blood oxygen sensor module. Max30102 integrates a red LED and infrared LED, photodetector, optical device, and low-noise electronic circuit with ambient light suppression. 

1. Measures Sleep stages using heart rate variability

(Going by a few research papers) During sleep, the human heart goes through high heart rate variability. This allows us to distinguish between stages at times. Observing this variability gives us the opportunity to create the SleepSmart pillow.

2. Set the Preferred range of wake-up time

During sleep, the brain goes into stage 1 multiple times. Now, we don't want to wake the user in the middle of the night (not good); therefore, we let the user choose a wake-up time period, so that the user is woken up when in Stage 1 in that period.

3. Hard wake-up time after the range passed

If the user has not entered stage 1 in that period, then after the period ends, the vibrations do go off and will wake you up, no matter what.
