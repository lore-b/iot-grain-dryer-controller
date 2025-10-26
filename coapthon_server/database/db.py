import pymysql.cursors

# Classe Database che gestisce la connessione al database MySQL
class Database:
    _connection = None  # variabile privata di classe

    def __init__(self):
        return

    # Esegue il reset del database, creando un nuovo database chiamato "iot"
    def reset_database(self):
        
        # Crea una connessione al database MySQL
        connection = pymysql.connect(
            host="localhost",
            user="root",
            password="Coap@2025",
            cursorclass=pymysql.cursors.DictCursor
        )
        with connection.cursor() as cursor:
            cursor.execute("DROP DATABASE IF EXISTS iot")
            print("Database 'iot' eliminato.")
            cursor.execute("CREATE DATABASE iot")
            print("Database 'iot' creato.")
            connection.commit()
        connection.close()


    # Restituisce la connessione al database, creando una nuova connessione se non esiste
    def connect_db(self):
        return pymysql.connect(
            host="localhost",
            user="root",
            password="Coap@2025",
            database="iot",
            cursorclass=pymysql.cursors.DictCursor,
            autocommit=False
        )
        
    # Chiude la connessione al database se esiste
    def close(self):
        if Database._connection is not None:
            Database._connection.close()
            Database._connection = None
            print("Connessione chiusa.")
