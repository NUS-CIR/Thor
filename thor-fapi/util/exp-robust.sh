#!/bin/bash

# check if the correct number of arguments is provided
if [ $# -ne 3 ]; then
    echo "Usage: $0 <UE_RNTI_1 in hex> <UE_RNTI_2 in hex> <UE_RNTI_3 in hex>"
    exit 1
fi

# convert the arguments which are hex strings to decimal
UE_RNTI_1=$((16#$1))
UE_RNTI_2=$((16#$2))
UE_RNTI_3=$((16#$3))

# Initialize state for each UE (0 = off, 1 = on)
CURR_1=0
CURR_2=0
CURR_3=0

# Set the duration in seconds
DURATION=10

# Calculate the end time
END_TIME=$((SECONDS + DURATION))

# Print starting UNIX timestamp
echo "Start timestamp: $(date +%s)"
echo "Starting execution for $DURATION seconds..."
echo "UEs: $UE_RNTI_1, $UE_RNTI_2, $UE_RNTI_3"

# Loop until the current SECONDS reaches the END_TIME
COUNT=0
while [ $SECONDS -lt $END_TIME ]; do
    # Randomly select one UE to flip
    RANDOM_UE=$((RANDOM % 3 + 1))

    if [ $RANDOM_UE -eq 1 ]; then
        CURR_1=$((1 - CURR_1))
        /home/cirlab/xzk/thor-project/build/control_client migrate $UE_RNTI_1 $CURR_1
        echo "Executed: migrate $UE_RNTI_1 $CURR_1"
    elif [ $RANDOM_UE -eq 2 ]; then
        CURR_2=$((1 - CURR_2))
        /home/cirlab/xzk/thor-project/build/control_client migrate $UE_RNTI_2 $CURR_2
        echo "Executed: migrate $UE_RNTI_2 $CURR_2"
    else
        CURR_3=$((1 - CURR_3))
        /home/cirlab/xzk/thor-project/build/control_client migrate $UE_RNTI_3 $CURR_3
        echo "Executed: migrate $UE_RNTI_3 $CURR_3"
    fi

    # Sleep for 500 microseconds (0.0005 seconds)
    # sleep 0.0005
    # Sleep for 10 slots, assuming 1 slot = 0.0005 seconds
    sleep 0.005
    COUNT=$((COUNT + 1))
done

/home/cirlab/xzk/thor-project/build/control_client migrate $UE_RNTI_1 0
/home/cirlab/xzk/thor-project/build/control_client migrate $UE_RNTI_2 0
/home/cirlab/xzk/thor-project/build/control_client migrate $UE_RNTI_3 0

echo "Finished. Total migrations: $COUNT."
# Print ending UNIX timestamp
echo "End timestamp: $(date +%s)"