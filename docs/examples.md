#### the cashbox of the validator is removed and then put back:
```
"PUBLISH" "validator-event" "{\"event\":\"cashbox removed\"}"
"PUBLISH" "validator-event" "{\"event\":\"disabled\"}"
"PUBLISH" "validator-event" "{\"event\":\"cashbox removed\"}"
"PUBLISH" "validator-event" "{\"event\":\"cashbox removed\"}"
"PUBLISH" "validator-event" "{\"event\":\"cashbox removed\"}"
"PUBLISH" "validator-event" "{\"event\":\"cashbox removed\"}"
"PUBLISH" "validator-event" "{\"event\":\"cashbox removed\"}"
"PUBLISH" "validator-event" "{\"event\":\"cashbox removed\"}"
"PUBLISH" "validator-event" "{\"event\":\"cashbox removed\"}"
"PUBLISH" "validator-event" "{\"event\":\"cashbox removed\"}"
"PUBLISH" "validator-event" "{\"event\":\"cashbox removed\"}"
"PUBLISH" "validator-event" "{\"event\":\"cashbox removed\"}"
"PUBLISH" "validator-event" "{\"event\":\"cashbox removed\"}"
"PUBLISH" "validator-event" "{\"event\":\"cashbox removed\"}"
"PUBLISH" "validator-event" "{\"event\":\"cashbox removed\"}"
"PUBLISH" "validator-event" "{\"event\":\"cashbox removed\"}"
"PUBLISH" "validator-event" "{\"event\":\"cashbox removed\"}"
"PUBLISH" "validator-event" "{\"event\":\"cashbox replaced\"}"
```

#### the hopper successfully pays out 6 euros (600 cent):
```
"publish" "hopper-request" "{\"msgId\":\"1234\",\"cmd\":\"do-payout\",\"amount\":600}"
"PUBLISH" "hopper-response" "{\"correlId\":\"1234\",\"result\":\"ok\"}"
"PUBLISH" "hopper-event" "{\"event\":\"dispensing\",\"amount\":0}"
"PUBLISH" "hopper-event" "{\"event\":\"dispensing\",\"amount\":400}"
"PUBLISH" "hopper-event" "{\"event\":\"dispensing\",\"amount\":400}"
"PUBLISH" "hopper-event" "{\"event\":\"dispensing\",\"amount\":400}"
"PUBLISH" "hopper-event" "{\"event\":\"dispensing\",\"amount\":600}"
"PUBLISH" "hopper-event" "{\"event\":\"dispensing\",\"amount\":600}"
"PUBLISH" "hopper-event" "{\"event\":\"cashbox paid\",\"amount\":0,\"cc\":\"EUR\"}"
"PUBLISH" "hopper-event" "{\"event\":\"dispensed\",\"amount\":600}"
```

#### 2 euros can't be paid out exactly:
```
"PUBLISH" "hopper-event" "{\"event\":\"coin credit\",\"amount\":200,\"cc\":\"EUR\"}"
"publish" "hopper-request" "{\"msgId\":\"1234\",\"cmd\":\"do-payout\",\"amount\":100}"
"PUBLISH" "hopper-response" "{\"correlId\":\"1234\",\"error\":\"can't pay exact amount\"}"
```

#### the hopper should pay out 10 euro in coins but cant because there is too few money available:
```
"publish" "hopper-request" "{\"msgId\":\"1234\",\"cmd\":\"do-payout\",\"amount\":1000}"
"PUBLISH" "hopper-response" "{\"correlId\":\"1234\",\"error\":\"not enough value in smart payout\"}"
```

#### the hopper has detected that 3 coins have been inserted (a 10 cent, 50 cent and a 2 euro coin):
```
"PUBLISH" "hopper-event" "{\"event\":\"coin credit\",\"amount\":10,\"cc\":\"EUR\"}"
"PUBLISH" "hopper-event" "{\"event\":\"coin credit\",\"amount\":50,\"cc\":\"EUR\"}"
"PUBLISH" "hopper-event" "{\"event\":\"coin credit\",\"amount\":200,\"cc\":\"EUR\"}"
```

#### the type of note in channel 2 is being inhibited (this means they should not be accepted by the hardware) and a 10 euro note (which corresponds to channel 2) is inserted afterwards. the validator declines the banknote and rejects it. at the end a request is sent to get the reason of the rejection of the last note.
```
"publish" "validator-request" "{\"msgId\":\"1234\",\"cmd\":\"inhibit-channels\",\"channels\":\"2\"}"
"PUBLISH" "validator-response" "{\"correlId\":\"1234\",\"result\":\"ok\"}"
"PUBLISH" "validator-event" "{\"event\":\"reading\"}"
"PUBLISH" "validator-event" "{\"event\":\"reading\"}"
"PUBLISH" "validator-event" "{\"event\":\"reading\"}"
"PUBLISH" "validator-event" "{\"event\":\"reading\"}"
"PUBLISH" "validator-event" "{\"event\":\"rejecting\"}"
"PUBLISH" "validator-event" "{\"event\":\"rejected\"}"
"publish" "validator-request" "{\"msgId\":\"1234\",\"cmd\":\"last-reject-note\"}"
"PUBLISH" "validator-response" "{\"correlId\":\"1234\",\"reason\":\"channel inhibited\",\"code\":6}"
```

#### a 10 euro note is inserted, detected and the amount credited:
```
"PUBLISH" "validator-event" "{\"event\":\"reading\"}"
"PUBLISH" "validator-event" "{\"event\":\"reading\"}"
"PUBLISH" "validator-event" "{\"event\":\"read\",\"amount\":1000,\"channel\":2}"
"PUBLISH" "validator-event" "{\"event\":\"stacking\"}"
"PUBLISH" "validator-event" "{\"event\":\"stacking\"}"
"PUBLISH" "validator-event" "{\"event\":\"credit\",\"amount\":1000,\"channel\":2}"
"PUBLISH" "validator-event" "{\"event\":\"stacking\"}"
"PUBLISH" "validator-event" "{\"event\":\"stacked\"}"
```

#### something has been inserted into the note validator but was rejected:
```
"PUBLISH" "validator-event" "{\"event\":\"reading\"}"
"PUBLISH" "validator-event" "{\"event\":\"reading\"}"
"PUBLISH" "validator-event" "{\"event\":\"rejecting\"}"
"PUBLISH" "validator-event" "{\"event\":\"rejecting\"}"
"PUBLISH" "validator-event" "{\"event\":\"rejecting\"}"
"PUBLISH" "validator-event" "{\"event\":\"rejected\"}"
```

#### the hopper is requested to test the payout of 5 euros, 10 euros, 20 euros and 50 euros:
```
"publish" "hopper-request" "{\"msgId\":\"1234\",\"cmd\":\"test-payout\",\"amount\":500}"
"PUBLISH" "hopper-response" "{\"correlId\":\"1234\",\"error\":\"not enough value in smart payout\"}"
"publish" "hopper-request" "{\"msgId\":\"1234\",\"cmd\":\"test-payout\",\"amount\":1000}"
"PUBLISH" "hopper-response" "{\"correlId\":\"1234\",\"error\":\"not enough value in smart payout\"}"
"publish" "hopper-request" "{\"msgId\":\"1234\",\"cmd\":\"test-payout\",\"amount\":2000}"
"PUBLISH" "hopper-response" "{\"correlId\":\"1234\",\"error\":\"not enough value in smart payout\"}"
"publish" "hopper-request" "{\"msgId\":\"1234\",\"cmd\":\"test-payout\",\"amount\":5000}"
"PUBLISH" "hopper-response" "{\"correlId\":\"1234\",\"error\":\"not enough value in smart payout\"}"
```
