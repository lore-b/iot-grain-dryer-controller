# -*- coding: utf-8 -*-
'''Preprocessing_Dataset.ipynb

Dataset by:  https://www.kaggle.com/datasets/taranvee/smart-home-dataset-with-weather-information

This notebook preprocesses the dataset, divides it into subsets, aggregates data, and prepares it for machine learning tasks.
'''

!pip install pandas
!pip install emlearn
!pip install -q keras-tuner

import pandas as pd
import numpy as np
import os
import io
import matplotlib.pyplot as plt
import keras_tuner as kt
import emlearn

from google.colab import drive
from scipy.stats import mode
from google.colab import files
from sklearn.model_selection import train_test_split
from sklearn.preprocessing import StandardScaler
from tensorflow.keras import Sequential
from tensorflow.keras.layers import Dense, Dropout
from tensorflow.keras.optimizers import Adam

# Carica il dataset
df = pd.read_csv('/content/drive/My Drive/HomeC.csv')

# Crea cartella di destinazione su Google Drive
drive_path = '/content/drive/MyDrive/dataset_diviso/'
os.makedirs(drive_path, exist_ok=True)

# Divide il dataset in 10 parti
chunks = np.array_split(df, 10)

# Salva ogni chunk come file CSV su Drive
for i, chunk in enumerate(chunks, start=1):
    file_path = os.path.join(drive_path, f'subset_{i}.csv')
    chunk.to_csv(file_path, index=False)
    print(f"Salvato: {file_path}")


# ------------ PREPROCESSING ----------------

#Carica file CSV
df = pd.read_csv("/content/drive/My Drive/dataset_diviso/subset_10.csv")

# Pulizia 'cloudCover'
df['cloudCover'] = df['cloudCover'].replace('cloudCover', 1).astype(float)

# Trasformazione della colonna "time" 
df['time'] = ((df['time'] - 1451624400) * 60) + 1451624400
df['time'] = pd.to_datetime(df['time'], unit='s')
df['time'] = df['time'].dt.strftime('%y-%m-%d-%H-%M-%S')

# Funzione moda robusta
def safe_mode(series):
    try:
        return series.mode().iloc[0]
    except:
        return np.nan

# Aggregazione ogni 15 righe (non per timestamp!)
def aggregate_every_n(df, n):
    grouped = []

    for i in range(0, len(df), n):
        chunk = df.iloc[i:i+n]
        if len(chunk) < n:
            break  # ignora ultimi record se incompleti

        row = {}

        # TIME: primo timestamp del blocco
        row['time'] = chunk['time'].iloc[0]

        # SOMMA consumi
        sum_cols = ['use [kW]', 'gen [kW]', 'House overall [kW]', 'Dishwasher [kW]',
                    'Furnace 1 [kW]', 'Furnace 2 [kW]', 'Home office [kW]',
                    'Fridge [kW]', 'Wine cellar [kW]', 'Garage door [kW]',
                    'Kitchen 12 [kW]', 'Kitchen 14 [kW]', 'Kitchen 38 [kW]',
                    'Barn [kW]', 'Well [kW]', 'Microwave [kW]', 'Living room [kW]',
                    'Solar [kW]']
        for col in sum_cols:
            row[col] = round(chunk[col].sum(), 9)

        # MEDIA meteo e sensori
        mean_cols = ['temperature']
        for col in mean_cols:
            row[col] = round(chunk[col].mean(), 9)

        # MODA per meteo testuale
        mode_cols = ['icon']
        for col in mode_cols:
            row[col] = safe_mode(chunk[col])

        # MEDIA meteo e sensori
        mean_cols = ['humidity', 'visibility']
        for col in mean_cols:
            row[col] = round(chunk[col].mean(), 9)

        # MODA per meteo testuale
        mode_cols = ['summary']
        for col in mode_cols:
            row[col] = safe_mode(chunk[col])

        # MEDIA meteo e sensori
        mean_cols = ['apparentTemperature', 'pressure', 'windSpeed',
                     'cloudCover', 'windBearing', 'precipIntensity',
                      'dewPoint', 'precipProbability']
        for col in mean_cols:
            row[col] = round(chunk[col].mean(), 9)

        grouped.append(row)

    return pd.DataFrame(grouped)

# Applica aggregazione
df_agg = aggregate_every_n(df, 60)

# Salva 
df_agg.to_csv('sub_aggr_10.csv', index=False)


# ----------------- DATASET AGGREGATO -----------------

# Carica tutti i file CSV 
uploaded = files.upload()

# unisco i file caricati in un unico DataFrame
df_list = []
for filename in uploaded.keys():
    df = pd.read_csv(filename)
    df_list.append(df)

# Ordina i file se necessario 
df_list_sorted = sorted(df_list, key=lambda x: x['time'].iloc[0])

# Concatenazione finale
df_finale = pd.concat(df_list_sorted, ignore_index=True)

# Salva il file unico
df_finale.to_csv('dataset_riunito.csv', index=False)
files.download('dataset_riunito_per_ora.csv')



# ----------------- ELIMINA COLONNE INUTILI ------------------
df = pd.read_csv("/content/drive/MyDrive/Obsoleti/dataset_riunito.csv")

# elimino colonne
df = df.drop(['use [kW]', 'gen [kW]', 'Dishwasher [kW]',  'Home office [kW]',  'Fridge [kW]', 'Wine cellar [kW]', 'Garage door [kW]', 'Kitchen 12 [kW]', 'Kitchen 14 [kW]', 'Kitchen 38 [kW]', 'Barn [kW]', 'Well [kW]', 'Microwave [kW]', 'Living room [kW]', 'icon', 'apparentTemperature', 'windBearing', 'dewPoint'], axis=1)
df.head()

# salva 
df.to_csv('dataset_riunito_pulito.csv', index=False)



# ----------------------- AGGIUNGI LE LABEL -----------------

df = pd.read_csv("/content/dataset_riunito_pulito.csv")

df['Difference [kW]'] = df['House overall [kW]'] - (df['Furnace 1 [kW]'] + df['Furnace 2 [kW]'])
df['Difference [kW]'] = df['Difference [kW]'].round(9)

# Aggiunta dei nuovi campi prendendo i valori dalla riga successiva
df['next_Difference [kW]'] = df['Difference [kW]'].shift(-1)
df['next_Solar [kW]'] = df['Solar [kW]'].shift(-1)

# Salva
df.to_csv("dataset_15_diff.csv", index=False)




# ----------------------- AGGIUNGI LABEL ULTIMA RIGA -----------------

df = pd.read_csv("/content/dataset_15_diff.csv")
tf = pd.read_csv("/content/drive/MyDrive/sub_aggr_10.csv")

# Ottieni l'ultimo record di subset_10
last_record_subset_10 = tf.tail(1)

# Ottieni l'indice dell'ultimo record in dataset_with_next_values
last_index_dataset = df.index[-1]

last_row = tf.iloc[-1]
difference = last_row['House overall [kW]'] - (last_row['Furnace 1 [kW]'] + last_row['Furnace 2 [kW]'])
difference = round(difference, 9)


# Inserisci i valori desiderati nell'ultimo record di dataset_with_next_values
df.loc[last_index_dataset, 'next_Difference [kW]'] = difference
df.loc[last_index_dataset, 'next_Solar [kW]'] = last_record_subset_10['Solar [kW]'].values[0]

# Visualizza l'ultimo record per verificare la modifica
print(df.tail(1))

# Salva il dataframe modificato (opzionale)
df.to_csv("dataset_15_diff_fine.csv", index=False)



#--------------- CONVERTO LA COLONNA TIME IN 3 COLONNE DIVERSE ---------------

df = pd.read_csv("/content/dataset_15_diff_fine.csv")

# Elimina le colonne specificate
df = df.drop(['summary', 'precipProbability'], axis=1)

# Converti la colonna 'time' in formato datetime (se non lo è già)
df['time'] = pd.to_datetime(df['time'], format='%y-%m-%d-%H-%M-%S')

# Estrai Mese, Giorno e Ora in nuove colonne
df['Mese'] = df['time'].dt.month
df['Giorno'] = df['time'].dt.day
df['Ora'] = df['time'].dt.hour

df = df.drop(['time'], axis=1)

# Ottieni l'elenco di tutte le colonne
cols = df.columns.tolist()

# Rimuovi le colonne 'Mese', 'Giorno', 'Ora' dalla loro posizione attuale
cols.remove('Mese')
cols.remove('Giorno')
cols.remove('Ora')

# Crea il nuovo ordine delle colonne, mettendo 'Mese', 'Giorno', 'Ora' all'inizio
new_cols_order = ['Mese', 'Giorno', 'Ora'] + cols

# Riorganizza il dataframe con il nuovo ordine delle colonne
df = df[new_cols_order]

# salvo
df.to_csv("dataset_with_next_values_cleaned.csv", index=False)



# ----------------- CALCOLO DIFFERENZA  -----------------

df = pd.read_csv("/content/dataset_with_next_values_cleaned.csv")
# calcola differenza
df['Difference [kW]'] = df['House overall [kW]'] - (df['Furnace 1 [kW]'] + df['Furnace 2 [kW]'])

# salvo
df.to_csv('dataset_con_differenza.csv', index=False)



# ------------- GRAFICI CONSUMO ORARIO ---------------

average_difference_per_hour = df.groupby('Ora')['House overall [kW]'].mean()

# Crea il grafico
plt.figure(figsize=(10, 6))
average_difference_per_hour.plot(kind='line', marker='o')

# Aggiungi titoli e etichette
plt.title('Media di Consumo per Ogni Ora')
plt.xlabel('Ora del Giorno')
plt.ylabel('Consumo [kW]')
plt.xticks(average_difference_per_hour.index) # Imposta le etichette sull'asse x per ogni ora
plt.grid(True)

# Mostra il grafico
plt.show()

df['somme [kW]'] =df['Furnace 1 [kW]'] + df['Furnace 2 [kW]']

average_difference_per_hour = df.groupby('Ora')['somme [kW]'].mean()

# Crea il grafico
plt.figure(figsize=(10, 6))
average_difference_per_hour.plot(kind='line', marker='o')

# Aggiungi titoli e etichette
plt.title('Media di Consumo per Ogni Ora')
plt.xlabel('Ora del Giorno')
plt.ylabel('Consumo [kW]')
plt.xticks(average_difference_per_hour.index) # Imposta le etichette sull'asse x per ogni ora
plt.grid(True)

# Mostra il grafico
plt.show()



# ------------ AGGIUNGO LA LABEL NEXT_DIFFERENCE [KW] -----------

df = pd.read_csv("/content/drive/MyDrive/dataset_con_differenza.csv")

df['Difference [kW]'] = df['Difference [kW]'].round(9)
df['next_Difference [kW]'] = df['Difference [kW]'].shift(-1)

# Elimina la colonna next_House overall [Kw]
df = df.drop(['next_House overall [kW]'], axis=1)

# salvo
df.to_csv("dataset_con_next_diff.csv", index=False)



# ----------------- AGGIUNGO LA LABEL NEXT_DIFFERENCE [KW] ALL'ULTIMA RIGA -----------------
# carico i due df
df_target = pd.read_csv('/content/dataset_con_next_diff.csv')
df_source = pd.read_csv('/content/drive/MyDrive/sub_aggr_10.csv')

# Calcolo della differenza all'ultima riga di df_source
last_row = df_source.iloc[-1]
difference = last_row['House overall [kW]'] - (last_row['Furnace 1 [kW]'] + last_row['Furnace 2 [kW]'])

# Inserisci il valore nel campo 'next_Difference [kW]' dell'ultima riga di df_target
df_target.at[df_target.index[-1], 'next_Difference [kW]'] = 26.72986667

# salvo
df_target.to_csv('dataset_con_next_diff.csv', index=False)




# --------- ADDESTRO I MODELLI -----------------
# Si tratta dello stesso codice per entrambi i modelli, ma con input e label differenti.
# Per semplicità si è scelto di mantenere un unico script per entrambi i casi, modificando solo le parti necessarie.


# Carica il dataset
df = pd.read_csv('/content/drive/MyDrive/nuovi_sub/dataset_aggr_consumo.csv')

# Feature & label
# features = df[['Difference [kW]', 'Mese', 'Ora',
#                'temperature', 'humidity']]                                # tot 5

features = df[['Solar [kW]', 'Mese', 'Ora',
               'temperature', 'humidity']]                                # tot 5

# labels = df['next_Difference [kW]']
labels = df['next_Solar [kW]']

# Normalizzazione
scaler = StandardScaler()
X_scaled = scaler.fit_transform(features)

# Train/test split
X_train, X_test, y_train, y_test = train_test_split(X_scaled, labels, test_size=0.2, random_state=42)

# Ricostruzione del modello con gli iperparametri migliori
model = Sequential()
model.add(Dense(units=48, activation='relu', input_shape=(X_train.shape[1],)))
model.add(Dense(units=80, activation='relu'))
model.add(Dense(units=80, activation='relu'))
model.add(Dense(units=32, activation='relu'))
model.add(Dense(units=80, activation='relu'))
model.add(Dense(units=48, activation='relu'))
model.add(Dropout(0.2))
model.add(Dense(1))

model.compile(optimizer=Adam(), loss='mse', metrics=['mae'])

# Addestramento del modello
history = model.fit(X_train, y_train, epochs=30, validation_split=0.2, verbose=1)

# Valutazione
loss, mae = model.evaluate(X_test, y_test)
print(f"\n Test MAE: {mae:.4f}")

# Predizione
y_pred = model.predict(X_test).flatten()

# Plot Actual vs Predicted
plt.figure(figsize=(8, 6))
plt.scatter(y_test, y_pred, alpha=0.5)
# Per next_difference
# plt.plot([0, 250], [0, 250], 'r--')
# plt.xlim(0, 250)
# plt.ylim(0, 250)

# Per next_Solar
plt.plot([0, 75], [0, 75], 'r--')
plt.xlim(0, 75)
plt.ylim(0, 75)

# plt.xlabel("Actual next_Difference [kW]")
plt.xlabel("Actual next_Solar [kW]")

plt.ylabel("Predicted")
plt.title("Actual vs Predicted - Manual Best Model")
plt.grid(True)
plt.show()



# ------------- ESTRAZIONE .h ---------------
# Anche qui si tratta dello stesso codice.


# path = 'prediction_next_diff.h'
path = 'prediction_next_solar.h'

cmodel = emlearn.convert(model, method='inline')

# cmodel.save(file=path, name='prediction_next_diff')
cmodel.save(file=path, name='prediction_next_solar')

print('Wrote model to', path)