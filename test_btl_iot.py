import numpy as np
import pandas as pd
import serial
from keras.models import Sequential
from keras.layers import Dense, Flatten, Conv1D, MaxPooling1D
import paho.mqtt.client as mqttclient
import time
import json

TOKEN = "4ten7qvaopjyof8jg7mt"  # Device token from CoreIoT/ThingsBoard
BROKER = "app.coreiot.io"       # MQTT Broker
PORT = 1883

# --- Keras Model Preparation ---
def split_sequences(sequences, n_steps):
    X, y = list(), list()
    for i in range(len(sequences)):
        end_ix = i + n_steps
        if end_ix > len(sequences)-1:
            break
        seq_x, seq_y = sequences[i:end_ix, :], sequences[end_ix, :]
        X.append(seq_x)
        y.append(seq_y)
    return np.array(X), np.array(y)

train_data = pd.read_csv(r"train.csv")
test_data = pd.read_csv(r"test.csv")

humi_seq_train = np.array(train_data['Relative_humidity_room'])
co2_seq_train = np.array(train_data['Indoor_temperature_room'])

humi_seq_train = humi_seq_train.reshape((len(humi_seq_train), 1))
co2_seq_train = co2_seq_train.reshape((len(co2_seq_train), 1))
dataset = np.hstack((humi_seq_train, co2_seq_train))
n_steps = 3
X, y = split_sequences(dataset, n_steps)
n_features = X.shape[2]

model = Sequential()
model.add(Conv1D(filters=64, kernel_size=2, activation='relu', input_shape=(n_steps, n_features)))
model.add(MaxPooling1D(pool_size=2))
model.add(Flatten())
model.add(Dense(50, activation='relu'))
model.add(Dense(n_features))
model.compile(optimizer='adam', loss='mse')
model.fit(X, y, epochs=100, verbose=1)

def prediction(array):
    x_input = np.array(array).reshape((1, n_steps, n_features))
    predicted_value = model.predict(x_input, verbose=0)
    print("Predicted humidity:", predicted_value[0][0])
    print("Predicted CO2:", predicted_value[0][1])
    print("-" * 20)
    temp_1 = float(predicted_value[0][1])
    humi_1 = float(predicted_value[0][0])
    return temp_1, humi_1

# --- MQTT Setup ---
def on_connect(client, userdata, flags, rc):
    if rc == 0:
        print("Connected successfully!!")
        client.subscribe("v1/devices/me/rpc/request/+")
    else:
        print("Connection failed")

def on_message(client, userdata, message):
    try:
        payload = json.loads(message.payload.decode())
        print("RPC received:", payload)
        if payload.get("method") == "setSwitch":
            switch_state = payload.get("params")
            ser.write((json.dumps({"switch": switch_state}) + "\n").encode())
            response_topic = message.topic.replace("request", "response")
            client.publish(response_topic, json.dumps({"switch": switch_state}), 1)
        # Add more RPC methods here if needed
    except Exception as e:
        print("RPC error:", e)

client = mqttclient.Client()
client.username_pw_set(TOKEN)
client.on_connect = on_connect
client.on_message = on_message
client.connect(BROKER, PORT)
client.loop_start()

# --- Serial Setup ---
ser = serial.Serial('COM3', 115200, timeout=2)

# --- Main Loop ---
array = [[0, 0, 0], [0, 0, 0]]  # [humidity][temp]
attemp = 0

while True:
    try:
        line = ser.readline().decode('utf-8').strip()
        if line.startswith("DATA:"):
            data = line.replace("DATA:", "").split(',')
            temp = float(data[0])
            humi = float(data[1])
            light = float(data[2])
            print("Received:", humi, temp, light)
        else:
            continue
    except Exception as e:
        print("Serial read error:", e)
        continue

    if attemp < 3:
        array[0][attemp] = humi
        array[1][attemp] = temp
        attemp += 1

    elif attemp == 3:
        attemp += 1
        temp_predict, humi_predict = prediction(array)
        collect_data = {
            'temperature': temp,
            'humidity': humi,
            'temperature_predict': temp_predict,
            'humidity_predict': humi_predict,
            'light': light
        }
        client.publish('v1/devices/me/telemetry', json.dumps(collect_data), 1)
    else:
        array = [[array[0][1], array[0][2], humi_predict], [array[1][1], array[1][2], temp_predict]]
        temp_predict, humi_predict = prediction(array)
        collect_data = {
            'temperature': temp,
            'humidity': humi,
            'temperature_predict': temp_predict,
            'humidity_predict': humi_predict,
            'light': light
        }
        client.publish('v1/devices/me/telemetry', json.dumps(collect_data), 1)

    print(array)
    time.sleep(10)