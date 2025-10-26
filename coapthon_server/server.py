from coapthon.server.coap import CoAP
from coapthon.client.helperclient import HelperClient
from coapthon.messages.request import Request
from coapthon.resources.resource import Resource
import threading
import time
from coapthon import defines
from database.db import Database
from datetime import datetime, timezone
import json
import math

DB = Database()     # istanza del database, per evitare di ricrearlo ogni volta


# Struttura in memoria per registrazioni
registered_nodes = []  # lista di dizionari con chiavi: id, ip, resource

# Variabili per il controllo della furnace
history_vector = [0]*24 # Vettore per tenere traccia dello stato della furnace nelle ultime 24 ore
max_on = 8  # Numero massimo di ore in cui la furnace può essere accesa in 24 ore
min_on = 4   # Numero minimo di ore in cui la furnace deve essere accesa in 24 ore
count_up_for = 0  # Contatore per il tempo in cui la furnace deve rimanere accesa
load_hour = 23  # Ora in cui la fornace viene caricata 
max_reached = 0  # Flag per indicare se il massimo di ore accese è stato raggiunto

# Observable var
furnace_status = None
ctrl_prediction = 1
last_edge_ctrl = 1  # Stato del controllo automatico della furnace prima di disabilitarlo


# Inserisco /res_data e /res_prediction come risorse disponibili
registered_nodes.append({
    "id": "server",
    "ip": "fd00::1",
    "resource": "/res_data"
})
registered_nodes.append({
    "id": "server",
    "ip": "fd00::1",
    "resource": "/res_prediction"
})

# === /res_data ===
class ResData(Resource):
    def __init__(self, name="res_data", coap_server=None):
        super(ResData, self).__init__(name, coap_server)
        self.payload = "{}"
        self.db = DB
        conn = self.db.connect_db()
        cursor = conn.cursor()

        # la tabella viene creata una sola volta nel costruttore
        cursor.execute('''
            CREATE TABLE IF NOT EXISTS res_data (
                id INT AUTO_INCREMENT PRIMARY KEY,
                time_sec BIGINT UNSIGNED,
                solar FLOAT, mese INT, ora INT,
                temperature FLOAT, humidity FLOAT, power FLOAT
            )
        ''')
        conn.commit()
        
    # Metodo che gestisce le richieste POST alla risorsa /res_data: riceve i dati dal nodo, li salva nel database e aggiorna il controllo anti-starvation della furnace.
    def render_POST(self, request):
        
        try:
            data = json.loads(request.payload)
            print("[/res_data] Dati ricevuti")
            # print("[/res_data] Ricevuto:", data)

            # Inserimento dati nel database
            conn = self.db.connect_db()
            cursor = conn.cursor()
            cursor.execute('''
                INSERT INTO res_data (time_sec, solar, mese, ora, temperature, humidity, power)
                VALUES (%s, %s, %s, %s, %s, %s, %s)
            ''', (
                to_epoch_seconds(data["ts"]), data["sol"], data["mese"], data["ora"],
                data["temp"], data["hum"], data["pow"]
            ))
            conn.commit()

            # Controllo per evitare la starvation della furnace
            avoid_starvation(data["ora"])

            self.payload = "OK"
        except Exception as e:
            print("[ERROR /res_data]", e)
            self.payload = "ERROR"
        finally:
            if cursor:
                try: cursor.close()
                except Exception: pass
            if conn and conn.open:          # chiudi solo se on
                try: conn.close()
                except Exception: pass
        return self


# === /res_prediction ===
class ResPrediction(Resource):
    def __init__(self, name="res_prediction", coap_server=None):
        super(ResPrediction, self).__init__(name, coap_server)
        self.payload = "{}"
        self.db = DB
        conn = self.db.connect_db()
        cursor = conn.cursor()

        # creazione tabella spostata nel costruttore
        cursor.execute('''
            CREATE TABLE IF NOT EXISTS res_prediction (
                id INT AUTO_INCREMENT PRIMARY KEY,
                time_sec BIGINT UNSIGNED,
                next_power FLOAT, next_solar FLOAT,
                missing INT
            )
        ''')
        conn.commit()
        
    # Gestisce POST su /res_prediction e inserisce i dati JSON nel database
    def render_POST(self, request):
        
        try:
            data = json.loads(request.payload)
            print("[/res_prediction] Dati ricevuti")
            # print("[/res_prediction] Ricevuto:", data)

            # Inserimento dati nel database
            conn = self.db.connect_db()
            cursor = conn.cursor()
            cursor.execute('''
                INSERT INTO res_prediction (time_sec, next_power, next_solar, missing)
                VALUES (%s, %s, %s, %s)
            ''', (
                to_epoch_seconds(data["ts"]), data["nPow"], data["nSol"], data["miss"]
            ))
            conn.commit()

            self.payload = "OK"
        except Exception as e:
            print("[ERROR /res_prediction]", e)
            self.payload = "ERROR"
        finally:
            if cursor:
                try: cursor.close()
                except Exception: pass
            if conn and conn.open:          # chiudi solo se davvero viva
                try: conn.close()
                except Exception: pass
        return self

# === /register ===
class RegisterResource(Resource):
    def __init__(self, name="register", coap_server=None):
        super(RegisterResource, self).__init__(name, coap_server)
        self.payload = "Registration endpoint"
        self.db = DB
        conn = self.db.connect_db()
        cursor = conn.cursor()

        # creazione tabella spostata nel costruttore
        cursor.execute('''
            CREATE TABLE IF NOT EXISTS nodes (
                        id INT AUTO_INCREMENT PRIMARY KEY,
                        node_id VARCHAR(64) NOT NULL,
                        node_ip VARCHAR(64) NOT NULL,
                        resource VARCHAR(64) NULL
            )
        ''')

        # inserisci le due risorse iniziali
        resources = ["/res_data", "/res_prediction"]
        for res in resources:
            cursor.execute(
                "INSERT INTO nodes (node_id, node_ip, resource) VALUES (%s, %s, %s)",
                ("server", "fd00::1", res)
            )
        conn.commit()

    # Gestisce le richieste POST su /register per registrare un nodo nel db e nella memoria
    def render_POST(self, request):
        print("[DEBUG] Sono entrato in render_POST di /register")

        try:
            data = json.loads(request.payload)
            print("[/register] Richiesta ricevuta:", data)

            node_id = data.get("id")
            if not node_id:
                self.code = defines.Codes.BAD_REQUEST.number
                self.payload = ""
                return self

            ip = request.source[0]
            resources = data.get("resources", [])

            if not isinstance(resources, list):
                self.code = defines.Codes.BAD_REQUEST.number
                self.payload = ""
                return self

            # Verifica se il nodo (IP) è già registrato
            already_registered = any(entry["ip"] == ip for entry in registered_nodes)

            # Se il nodo non è già registrato, lo aggiungo alla lista
            if not already_registered:
                if resources:
                    for res in resources:
                        registered_nodes.append({
                            "id": node_id,
                            "ip": ip,
                            "resource": res
                        })
                        
                else:
                    registered_nodes.append({
                        "id": node_id,
                        "ip": ip,
                        "resource": None
                    })
                print(f"[*] Nodo {node_id} registrato da IP {ip}")

                # Inserimento nel database
                conn = self.db.connect_db()
                cursor = conn.cursor()
                for res in resources:
                    cursor.execute('''
                            INSERT INTO nodes (node_id, node_ip, resource)
                            VALUES (%s, %s, %s)
                        ''', (
                                node_id, ip, res
                        ))
                conn.commit()

            else:
                print(f"[!] Nodo già registrato da IP {ip}, registrazione ignorata.")

            print("[MEMORIA] Stato attuale:")
            for entry in registered_nodes:
                print("  -", entry)
        except json.JSONDecodeError:
            self.code = defines.Codes.BAD_REQUEST.number
            self.payload = ""
        except Exception as e:
            print("[ERROR /register]", e)
            self.code = defines.Codes.INTERNAL_SERVER_ERROR.number
            self.payload = ""
        finally:
            if cursor:
                try: cursor.close()
                except Exception: pass
            if conn and conn.open:          # chiudi solo se davvero viva
                try: conn.close()
                except Exception: pass
        return self
    
    # Gestisce le richieste GET su /register per sincronizzare il timestamp
    def render_GET(self, request):
        print("[DEBUG] GET ricevuta su /register per sync timestamp")
        try:
            current_time = str(int(time.time()))
            self.code = defines.Codes.CONTENT.number  # 2.05
            self.payload = json.dumps({"timestamp": current_time})
            self.content_type = defines.Content_types["application/json"]  
            print("[DEBUG] Inviato timestamp:", self.payload)
        except Exception as e:
            print("[ERROR /register GET]", e)
            self.code = defines.Codes.INTERNAL_SERVER_ERROR.number
            self.payload = ""
        return self


# === /lookup ===
class LookupResource(Resource):
    def __init__(self, name="lookup", coap_server=None):
        super(LookupResource, self).__init__(name, coap_server)
        self.payload = "{}"
        self.db = DB
        
    # Gestisce le richieste GET su /lookup per cercare l'IP di una risorsa registrata
    def render_GET(self, request):
        try:
            self.code = 0.00  # Inizializzo
            query = request.uri_query
            resource_requested = None

            query = request.uri_query 
            resource_requested = None

            if query:
                for param in query.split("&"):
                    if param.startswith("res="):
                        resource_requested = param.split("=", 1)[1]
                        break

            # Se il parametro res è assente o vuoto
            if resource_requested is None or resource_requested.strip() == "":
                self.code = defines.Codes.BAD_REQUEST.number  # 4.00
                self.payload = ""
                print("[DEBUG] Risorsa richiesta non specificata")
                return self

            # Cerca la risorsa tra i nodi registrati
            print(f"[DEBUG] Risorsa richiesta: {resource_requested}")
            for entry in registered_nodes:
                
                if entry["resource"] == resource_requested:
                    # Risorsa trovata, restituisci l'IP
                    self.code = defines.Codes.CONTENT.number  # 2.05
                    self.payload = json.dumps({
                        "ip": entry["ip"]
                    })
                    print("[DEBUG] Risorsa trovata!")
                    return self

            # Cerca la risorsa tra i nodi registrati sul db
            # conn = self.db.connect_db()
            # cursor = conn.cursor()
            # cursor.execute('''
            #     SELECT node_ip FROM nodes WHERE resource = %s
            # ''', (resource_requested,))
            # result = cursor.fetchone()
            
            # print(f"[DEBUG] Risorsa richiesta: {resource_requested} | Risultato DB: {result}")

            # if result:
            #     self.code = defines.Codes.CONTENT.number  # 2.05
            #     self.payload = json.dumps({
            #         "ip": result[0]
            #     })
            #     print("[DEBUG] Risorsa trovata nel DB!")
            # else:
            #     # Risorsa non trovata
            #     print("[DEBUG] Risorsa NON trovata nel DB")
            #     self.code = defines.Codes.NOT_FOUND.number
            #     self.payload = ""
            
        except Exception as e:
            print("[ERROR /lookup]", e)
            self.code = defines.Codes.INTERNAL_SERVER_ERROR.number  # 5.00
            self.payload = ""
        # finally:
        #     if cursor:
        #         try: cursor.close()
        #         except Exception: pass
        #     if conn and conn.open:          # chiudi solo se davvero viva
        #         try: conn.close()
        #         except Exception: pass
        return self

# === /starving ===
class StarvationResource(Resource):
    def __init__(self, name="StarvationResource", coap_server=None):
        super(StarvationResource, self).__init__(name, coap_server, visible=True, observable=False, allow_children=False)
        self.payload = "Starvation configuration endpoint"

    # Gestisce le richieste GET e PUT su /starvation per ottenere e aggiornare la configurazione della furnace
    def render_GET(self, request):
        # Restituisce la configurazione corrente della furnace al client
        self.payload = json.dumps({
            "max_on": max_on,
            "min_on": min_on,
            "load_hour": load_hour
        })

        self.content_type = defines.Content_types["application/json"] 
        self.code = defines.Codes.CONTENT.number  # 2.05
        return self

    # Gestisce le richieste PUT su /starvation per aggiornare la configurazione della furnace
    def render_PUT(self, request):
        global max_on, min_on, load_hour

        try:
            data = json.loads(request.payload)

            if "max_on" in data:
                max_on = int(data["max_on"])
                print("[REMOTO] max_on aggiornato a", max_on)
            if "min_on" in data:
                min_on = int(data["min_on"])
                print("[REMOTO] min_on aggiornato a", min_on)
            if "load_hour" in data:
                load_hour = int(data["load_hour"])
                print("[REMOTO] load_hour aggiornato a", load_hour)

            self.payload = json.dumps({
                "status": "updated",
                "max_on": max_on,
                "min_on": min_on,
                "load_hour": load_hour
            })
            print(f"Updated config: max_on={max_on}, min_on={min_on}, load_hour={load_hour}")
        except Exception as e:
            self.payload = json.dumps({"error": str(e)})
            print(f"Error parsing POST data: {e}")

        return self
    
# Funzione di callback per le notifiche della furnace
def furnace_notification_callback(response):
    global furnace_status
    try:
        data = json.loads(response.payload)
        if "furnace_state" in data:
            furnace_status = int(data["furnace_state"])
            state_str = "ACCESA" if furnace_status else "SPENTA"
            print(f" [NOTIFICA] Furnace {state_str} (remota)")
        else:
            print("[!] JSON ricevuto ma senza 'furnace_state':", data)
    except Exception as e:
        print("[!] Payload sconosciuto:", response.payload, "| errore:", e)

# Funzione per iscriversi alla risorsa osservabile /res_furnace
def observe_remote_furnace(ip):
    print(f"[*] Mi iscrivo alla risorsa osservabile /res_furnace su nodo [{ip}]")
    client = HelperClient(server=(ip, 5683))

    request = Request()
    request.code = defines.Codes.GET.number
    request.uri_path = "res_furnace"
    request.observe = 0  # 0 = registrazione
    request.token = b'4567'
    request.destination = (ip, 5683)

    client.send_request(request, callback=furnace_notification_callback)

# Funzione che attende la registrazione della risorsa /res_furnace e avvia l'osservazione
def watch_furnace_resource():
    print("[*] In attesa che /res_furnace venga registrata...")

    while True:
        for entry in registered_nodes:
            if entry["resource"] == "/res_furnace":
                ip = entry["ip"]
                print(f"[✓] Trovata /res_furnace su nodo [{ip}] Avvio osservazione")
                observe_remote_furnace(ip)

                # Avvio il logger per lo stato della furnace
                threading.Thread(target=start_furnace_logger, daemon=True).start()
                return
        time.sleep(2)  # Ricontrolla ogni 2 secondi

# Funzione che avvia il logger per registrare lo stato della furnace periodicamente
def start_furnace_logger():
    
    print("[LOGGER] Avvio registrazione periodica stato furnace...")

    # creo tabella
    try:
        with DB.connect_db() as conn, conn.cursor() as cur:
            cur.execute("""
                CREATE TABLE IF NOT EXISTS furnace_log (
                    id INT AUTO_INCREMENT PRIMARY KEY,
                    time_sec BIGINT UNSIGNED,
                    status INT
                )
            """)
            conn.commit()
            print("[LOGGER] Tabella furnace_log pronta.")
    except Exception as e:
        print("[LOGGER ERROR - init]", e)
        return                         

    # loop di registrazione su db dello stato della furnace
    while True:
        timestamp = str(int(time.time()))
        status = 1 if furnace_status else 0      

        try:
            with DB.connect_db() as conn, conn.cursor() as cur:
                cur.execute(
                    "INSERT INTO furnace_log (time_sec, status) VALUES (%s, %s)",
                    (timestamp, status)
                )
                conn.commit()
                print(f"[LOGGER] Dati Furnace salvati: timestamp={timestamp} , status_furnace={status}")

        except Exception as e:
            print("[LOGGER ERROR]", e)

        time.sleep(15)


# Funzione di callback per le notifiche della risorsa /res_threshold
def threshold_callback(response):
    global ctrl_prediction
    payload = response.payload
    if isinstance(payload, bytes):          
        payload = payload.decode("utf-8")

    try:
        data = json.loads(payload)

        if "auto_furnace_ctrl" in data:
            ctrl_prediction = int(data["auto_furnace_ctrl"])
            state = "ON" if ctrl_prediction else "OFF"
            print(f"[NOTIFICA] Controllo Furnace Automatico {state} (remoto)")
        else:
            print("[!] JSON senza 'auto_furnace_ctrl':", data)

    except Exception as e:
        print("[!] Payload sconosciuto:", response.payload, "| errore:", e)


# Funzione per iscriversi alla risorsa osservabile /res_threshold
def observe_remote_threshold(ip):
    print(f"[*] Mi iscrivo alla risorsa osservabile /res_threshold su nodo [{ip}]")
    client = HelperClient(server=(ip, 5683))

    request = Request()
    request.code = defines.Codes.GET.number
    request.uri_path = "res_threshold"
    request.observe = 0  # 0 = registrazione
    request.token = b'9876'
    request.destination = (ip, 5683)

    client.send_request(request, callback=threshold_callback)


# Funzione che attende la registrazione della risorsa /res_threshold e avvia l'osservazione
def watch_threshold_resource():
    print("[*] In attesa che /res_threshold venga registrata...")

    while True:
        for entry in registered_nodes:
            if entry["resource"] == "/res_threshold":
                ip = entry["ip"]
                print(f"[✓] Trovata /res_threshold su nodo [{ip}] Avvio osservazione")
                observe_remote_threshold(ip)
                return  # termina il thread una volta avviata l’osservazione
        time.sleep(2)



# Funzione per convertire un timestamp in secondi 
def to_epoch_seconds(raw_ts):
    """
    Converte in int(epoch seconds) accettando:
      • stringa 'YYYY-MM-DD HH:MM:SS'
      • ISO-8601 ('2025-06-12T14:23:00Z', con o senza offset)
      • numero/ stringa epoch (s o ms)
    """

    if isinstance(raw_ts, (int, float)):
        epoch = float(raw_ts)
    elif str(raw_ts).strip().isdigit():
        epoch = float(raw_ts)
    else:
        epoch = None

    if epoch is not None:               
        if epoch > 1e12:                
            epoch /= 1000.0
        return int(epoch)

    # 'YYYY-MM-DD HH:MM:SS' 
    try:
        dt = datetime.strptime(raw_ts, "%Y-%m-%d %H:%M:%S")
    except ValueError:
        # ISO-8601 
        dt = datetime.fromisoformat(str(raw_ts).replace("Z", "+00:00"))

    if dt.tzinfo is None:            
        dt = dt.replace(tzinfo=timezone.utc)
    else:
        dt = dt.astimezone(timezone.utc)

    return int(dt.timestamp())


# Funzione per ottenere l'IP di un nodo registrato in base alla risorsa richiesta
def get_ip(resource_requested):
    for entry in registered_nodes:
        if entry["resource"] == resource_requested:
            # Risorsa trovata, restituisci l'IP
            return entry["ip"]
    return None  # Risorsa non trovata


# Funzione per inviare un comando PUT al nodo specificato
def send_put(ip, resource, payload):
    print(f"[PUT] Invia comando a {resource} su nodo [{ip}]")
    client = HelperClient(server=(ip, 5683))
    response = client.put(resource, json.dumps(payload))
    client.stop()

    # Risposta del nodo (codice 68 vuol dire OK)
    if response:
        print("Risposta:", response.code)
    else:
        print("Nessuna risposta")        


# Funzione per evitare la starvation della furnace
def avoid_starvation(current_hour):
    global history_vector, count_up_for, max_on, min_on, load_hour, max_reached, furnace_status, last_edge_ctrl, ctrl_prediction
    # Funzione per evitare che la furnace resti sempre spenta o sempre accesa
    
    if current_hour == load_hour:
        history_vector = [0] * 24 # Resetto vettore, fornace appena caricata

    status_val = 1 if furnace_status else 0
    history_vector.pop(0) # Shifto il vettore orario
    history_vector.append(status_val)

    total_on = sum(history_vector)  # Conta quante volte la furnace è stata accesa nelle 24 ore
    remain = min_on - total_on  # Ore rimanenti da accendere per raggiungere il minimo

    # Se contatore attivo, controllo se spegnerlo (se ho raggiunto il tempo minimo) e riabilito controllo automatico
    if count_up_for > 0:
        count_up_for -= 1
        if count_up_for <= 0:
            ip = get_ip("/res_threshold")
            if not ip:
                print("[!] Risorsa /res_threshold non trovata, impossibile riaccendere edge")
                return
            send_put(ip, "res_threshold", {"auto_furnace_ctrl": last_edge_ctrl})
            print("[AUTOCONTROL] Riabilitato controllo automatico della furnace")
            return
        else:
            return


    if total_on  >= max_on:
        # Se la furnace è stata accesa troppo tempo, spegni e impedisci accensione automatica
        ip = get_ip("/res_threshold")
        if not ip:
            print("[!] Risorsa /res_threshold non trovata, impossibile spegnere edge")
            return
        send_put(ip, "res_threshold", {"auto_furnace_ctrl": 0})
        print("[AUTOCONTROL] Disabilitato controllo automatico della furnace, raggiunto tempo massimo di accensione")

        ip = get_ip("/res_furnace")
        if not ip:
            print("[!] Risorsa /res_furnace non trovata, impossibile spegnere fornace")
            return
        send_put(ip, "res_furnace", {"furnace_state": 0})
        print("[FURNACE] Furnace spenta per raggiungimento tempo massimo di accensione")
        
        max_reached = True
        return

    else:
        # economy_window = [(load_hour + i) % 24 for i in range(3)]
        must_turnon = (((load_hour - remain)) % 24 + 24) % 24  # Ora in cui la furnace deve essere accesa per raggiungere il minimo
        if current_hour == must_turnon and total_on < min_on:
            # Accendi la furnace se è l'ora più economica e non è stata accesa abbastanza nelle ultime 24 ore. 
            # Avvio contatore, disabilito controllo automatico, accendo furnace
            count_up_for = remain 
            last_edge_ctrl = ctrl_prediction  # Salvo lo stato del controllo automatico prima di disabilitarlo

            ip = get_ip("/res_threshold")
            if not ip:
                print("[!] Risorsa /res_threshold non trovata, impossibile spegnere edge")
                return
            send_put(ip, "res_threshold", {"auto_furnace_ctrl": 0})
            print("[AUTOCONTROL] Disabilitato controllo automatico della furnace")

            ip = get_ip("/res_furnace")
            if not ip:
                print("[!] Risorsa /res_furnace non trovata, impossibile spegnere fornace")
                return
            send_put(ip, "res_furnace", {"furnace_state": 1})
            print("[FURNACE] Furnace accesa  da server")
            return
        elif max_reached == True: 
            # Il controllo automatico era stato disabilitato perchè raggiunte ore massime, adesso posso riaccendere fornace quando conveniente
            ip = get_ip("/res_threshold")
            if not ip:
                print("[!] Risorsa /res_threshold non trovata, impossibile spegnere edge")
                return
            send_put(ip, "res_threshold", {"auto_furnace_ctrl": 1})
            print("[AUTOCONTROL] Riabilitato controllo automatico della furnace")
            max_reached = False
            return





# === Server ===
class CoAPServer(CoAP):
    def __init__(self, host, port):
        CoAP.__init__(self, (host, port), False)
        db = DB
        db.reset_database()     
        self.add_resource("res_data/", ResData())
        self.add_resource("res_prediction/", ResPrediction())
        self.add_resource("register/", RegisterResource())
        self.add_resource("lookup/", LookupResource())
        self.add_resource('starvation/', StarvationResource())
        print(f"[SERVER] In ascolto su coap://[{host}]:{port}")


if __name__ == '__main__':
    ip = "::"   # IP del server nella rete dei sensori
    port = 5683
    server = CoAPServer(ip, port)

    # Attende che la risorsa /res_furnace venga registrata e avvia l'osservazione
    observer_thread_a = threading.Thread(target=watch_furnace_resource)
    observer_thread_a.daemon = True
    observer_thread_a.start()

    # Attende che la risorsa /res_threshold venga registrata e avvia l'osservazione
    observer_thread_b = threading.Thread(target=watch_threshold_resource)
    observer_thread_b.daemon = True
    observer_thread_b.start()

    try:
        # Server in ascolto
        server.listen(10)
    except KeyboardInterrupt:
        print("Arresto server...")
        server.close()
