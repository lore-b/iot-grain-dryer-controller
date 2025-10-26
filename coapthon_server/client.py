import json
from coapthon.client.helperclient import HelperClient

SERVER_IP = "fd00::1"
SERVER_PORT = 5683


# Funzioni per interagire con il server CoAP
def lookup_resource(resource):
    print(f"[LOOKUP] Cerco IP per la risorsa {resource}")
    client = HelperClient(server=(SERVER_IP, SERVER_PORT))
    response = client.get(f"lookup?res={resource}")
    client.stop()

    if response and response.payload:
        try:
            data = json.loads(response.payload)
            return data.get("ip")
        except Exception as e:
            print("Errore parsing JSON:", e)
    return None

# Funzione per inviare un comando PUT al nodo specificato
def send_put(ip, resource, payload):
    print(f"[PUT] Invia comando a {resource} su nodo [{ip}]")
    client = HelperClient(server=(ip, SERVER_PORT))
    response = client.put(resource, json.dumps(payload))
    client.stop()

    # Risposta del nodo 
    if response:
        print("Risposta:", response.code)
    else:
        print("Nessuna risposta")

# Funzione per inviare una richiesta GET 
def coap_get(ip: str, resource: str):
    client = HelperClient(server=(ip, SERVER_PORT))
    try:
        resp = client.get(resource)
    finally:
        client.stop()
    return resp.payload if resp else None

# Funzioni per ottenere informazioni sullo stato della furnace
def get_furnace_state():
    ip = lookup_resource("/res_furnace")
    if not ip:
        return "? (node not found)"

    payload = coap_get(ip, "res_furnace")
    if not payload:
        return None

    try:
        data = json.loads(payload)
        return {
            "furnace_state": "ON" if data.get("furnace_state", 0) else "OFF"
        }
    except Exception:
        return None

# Funzione per ottenere informazioni sul nodo edge
def get_edge_info():
    ip = lookup_resource("/res_threshold")
    if not ip:
        return None  # edge not found

    payload = coap_get(ip, "res_threshold")
    if not payload:
        return None

    try:
        data = json.loads(payload)
        return {
            "auto": "ON" if data.get("auto_furnace_ctrl", 0) else "OFF",
            "thr_on":  data.get("on_threshold", "?"),
            "thr_off": data.get("off_threshold", "?")
        }
    except Exception:
        return None

# Funzione per ottenere informazioni sulla starvation
def get_starvation_info():
    payload = coap_get(SERVER_IP, "starvation")  
    if not payload:
        return None
    try:
        data = json.loads(payload)
        return {
            "max_on":       data.get("max_on", "?"),
            "min_on":       data.get("min_on", "?"),
            "load_hour": data.get("load_hour", "?")
        }
    except Exception:
        return None


def main():
    while True:
        print("\n--- COMANDI ---")
        print("1. Accendi furnace")
        print("2. Spegni furnace")
        print("3. Imposta soglia energia accensione")
        print("4. Imposta soglia energia spegnimento")
        print("5. Auto Controllo Furnace ON")
        print("6. Auto Controllo Furnace OFF")
        print("7. Modifica massima accensione giornaliera")
        print("8. Modifica minima accensione giornaliera")
        print("9. Imposta orario di carico")
        print("10. Info System")
        print("0. Esci")

        scelta = input("Seleziona comando: ").strip()

        if scelta == "0":
            print("Uscita dal programma.")
            break

        elif scelta in ["1", "2"]:
            ip = lookup_resource("/res_furnace")
            if not ip:
                print("Nodo furnace non trovato.")
                continue

            furnace_state = 1 if scelta == "1" else 0
            # Invia il comando di accensione/spegnimento
            print(f"Inviando comando '{furnace_state}' al nodo furnace...")
            send_put(ip, "res_furnace", {"furnace_state": furnace_state})

        elif scelta == "3":
            ip = lookup_resource("/res_threshold")
            if not ip:
                print("Nodo soglia non trovato.")
                continue
            try:
                threshold = int(input("Nuova soglia di potenza: "))
                # Invia il comando per impostare la soglia
                print(f"Inviando nuova soglia {threshold} al nodo edge...")
                send_put(ip, "res_threshold", {"threshold_on": threshold})
            except ValueError:
                print("Valore non valido.")

        elif scelta == "4":
            ip = lookup_resource("/res_threshold")
            if not ip:
                print("Nodo soglia non trovato.")
                continue
            try:
                threshold = int(input("Nuova soglia di potenza: "))
                # Invia il comando per impostare la soglia
                print(f"Inviando nuova soglia {threshold} al nodo edge...")
                send_put(ip, "res_threshold", {"threshold_off": threshold})
            except ValueError:
                print("Valore non valido.")

        elif scelta in ["5", "6"]:
            ip = lookup_resource("/res_threshold")
            if not ip:
                print("Nodo edge non trovato.")
                continue

            auto_furnace_ctrl = 1 if scelta == "5" else 0
            # Invia il comando di accensione/spegnimento
            print(f"Inviando comando '{auto_furnace_ctrl}' al nodo edge...")
            send_put(ip, "res_threshold", {"auto_furnace_ctrl": auto_furnace_ctrl})

        elif scelta in ["7", "8", "9"]:
            ip = SERVER_IP

            if scelta == "7":
                # Modifico max_on
                try:
                    max_on = int(input("Nuova soglia massima ore accesa: "))
                    if not (0 <= max_on <= 24):
                        print("Valore non valido. Inserire un numero tra 0 e 24.")
                    else:
                        # Invia il comando per impostare la soglia
                        print(f"Inviando nuova soglia massima di ore: {max_on} al server...")
                        send_put(ip, "starvation", {"max_on": max_on})
                except ValueError:
                    print("Valore non valido. Inserire un numero tra 0 e 24.")

            if scelta == "8":
                # Modifico min_on
                try:
                    min_on = int(input("Nuova soglia minima ore accesa: "))
                    if not (0 <= min_on <= 24):
                        print("Valore non valido. Inserire un numero tra 0 e 24.")
                    else:
                        # Invia il comando per impostare la soglia
                        print(f"Inviando nuova soglia minima di ore: {min_on} al server...")
                        send_put(ip, "starvation", {"min_on": min_on})
                except ValueError:
                    print("Valore non valido. Inserire un numero tra 0 e 24.")

            if scelta == "9":
                # Modifico load_hour
                try:
                    load_hour = int(input("Nuovo orario di carico: "))
                    if not (0 <= load_hour <= 23):
                        print("Valore non valido. Inserire un numero tra 0 e 23.")
                    else:
                        # Invia il comando per impostare l'ora
                        print(f"Inviando nuovo orario di carico: {load_hour} al server...")
                        send_put(ip, "starvation", {"load_hour": load_hour})
                except ValueError:
                    print("Valore non valido.")

        # elif scelta == "7":
            # Voglio stampare l'accensione di edge e furnace (che sono nel server)
            # e le soglie di accensione e spegnimento
        elif scelta == "10":
            furnace = get_furnace_state()
            edge    = get_edge_info()
            starv   = get_starvation_info()

            print("\n=== STATO SISTEMA ===")
            print("Furnace  → stato:", furnace["furnace_state"])

            if edge:
                threshold_cut = edge["thr_off"] + (edge["thr_off"] * 30) / 100
                print("Edge  → auto-mode:", edge["auto"],
                      "| threshold_on:", edge["thr_on"],
                      "| threshold_off:", edge["thr_off"],
                      "| threshold_cut:", threshold_cut)
            else:
                print("Edge  → ? (no data)")

            if starv:
                print("Starvation → max_on:", starv["max_on"],
                      "| min_on:", starv["min_on"],
                      "| load_hour:", starv["load_hour"])
            else:
                print("Starvation → ? (no data)")

            print("\n====================")
        else:
            print("Comando non riconosciuto.")

if __name__ == "__main__":
    main()
