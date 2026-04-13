#!/bin/bash
export CALL_RECORDING_ENABLE=1
export HTTP_PORT=18080

# 1. Start mock server
cd src/emulator
PYTHONPATH=. python3 mock_server.py > /tmp/mock_server.log 2>&1 &
MOCK_PID=$!
cd ../..

sleep 2

# 2. Start VBGW
./build/vbgw > /tmp/vbgw_test.log 2>&1 &
VBGW_PID=$!

sleep 3

# 3. Start SIP emulator with 2 parallel calls
cd src/emulator
python3 emulator.py test > /tmp/emulator.log 2>&1 &
EMU_PID=$!
cd ../..

# Let it run for 10 seconds to generate traffic
sleep 15

# 4. Extract metrics / API Call for Bridge (Simulation)
# We will just let the emulator finish or send DTMF if emulator.py does it

# 5. Graceful shutdown VBGW
kill -INT $VBGW_PID
wait $VBGW_PID 2>/dev/null

kill -TERM $MOCK_PID 2>/dev/null
kill -TERM $EMU_PID 2>/dev/null

echo "=== Mock Server Logs ==="
head -20 /tmp/mock_server.log
echo "=== Emulator Logs ==="
tail -20 /tmp/emulator.log
echo "=== VBGW Logs (grep Cleanup) ==="
grep "Cleanup" /tmp/vbgw_test.log
echo "=== VBGW Logs (grep Shutdown) ==="
grep "Shutdown" /tmp/vbgw_test.log
