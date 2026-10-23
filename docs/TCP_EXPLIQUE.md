# TCP expliqué simplement (et ce que voit PortScan-C)

## 1. Un port, c'est quoi ?

Une machine a une adresse IP (par exemple `127.0.0.1`). Sur cette machine,
plusieurs programmes peuvent attendre des connexions réseau en même temps.
Pour savoir à quel programme s'adresse une connexion, on utilise un **numéro
de port** (de 1 à 65535) : 22 pour SSH, 80 pour HTTP, 443 pour HTTPS...

Un programme qui attend des connexions sur un port est dit « en écoute ».
Un scanner de ports cherche quels ports sont en écoute sur une machine.

## 2. La poignée de main TCP (three-way handshake)

Avant d'échanger des données, TCP établit une connexion en trois messages :

```
   Client (PortScan-C)                         Serveur (port 22)
          |                                            |
          |  1. SYN      "je voudrais me connecter"    |
          |------------------------------------------->|
          |                                            |
          |  2. SYN-ACK  "d'accord, je suis là"        |
          |<-------------------------------------------|
          |                                            |
          |  3. ACK      "bien reçu, c'est parti"      |
          |------------------------------------------->|
          |                                            |
          |        connexion établie (données...)      |
          |<==========================================>|
```

- **SYN** (synchronize) : demande d'ouverture de connexion.
- **SYN-ACK** : le serveur accepte et confirme.
- **ACK** (acknowledge) : le client confirme à son tour.
- **RST** (reset) : message qui refuse ou coupe brutalement une connexion.

C'est le **système d'exploitation** qui envoie ces paquets, pas le
programme : quand PortScan-C appelle `connect()`, le noyau Linux envoie le
SYN et gère la suite.

## 3. Les trois cas vus par le scanner

### Cas 1 : port OUVERT (un programme écoute)

```
   PortScan-C                                  Cible
       |  SYN  ---------------------------------->  |
       |  <---------------------------------  SYN-ACK
       |  ACK  ---------------------------------->  |
       |                                            |
   connect() réussit  ->  getsockopt(SO_ERROR) = 0  ->  "ouvert"
```

Le scanner essaie ensuite de lire une **bannière** : beaucoup de services
envoient une ligne de présentation dès la connexion (par exemple SSH envoie
`SSH-2.0-OpenSSH_9.2`). Pour les ports web (80, 8000, 8080, 8888), le
scanner envoie `HEAD / HTTP/1.0` et lit la première ligne de la réponse
(`HTTP/1.0 200 OK`).

### Cas 2 : port FERMÉ (personne n'écoute, mais la machine répond)

```
   PortScan-C                                  Cible
       |  SYN  ---------------------------------->  |
       |  <-------------------------------------  RST
       |                                            |
   connect() échoue  ->  erreur ECONNREFUSED  ->  "fermé"
```

La machine existe et répond, mais aucun programme n'écoute sur ce port :
le noyau de la cible renvoie un **RST**. Côté scanner, l'erreur est
`ECONNREFUSED` (« connexion refusée »). C'est très rapide.

### Cas 3 : port FILTRÉ (pas de réponse du tout)

```
   PortScan-C                                  Pare-feu      Cible
       |  SYN  -------------------------------->  X  (paquet jeté)
       |                                          |
       |      ... silence ...                     |
       |                                          |
   select() attend "timeout_ms" et renvoie 0  ->  "filtré"
```

Un pare-feu jette le SYN sans rien répondre (règle DROP). Le scanner ne
reçoit ni SYN-ACK ni RST : il attend jusqu'au délai maximum (`-t`, 500 ms
par défaut) puis classe le port comme **filtré**. Les autres erreurs
réseau (`EHOSTUNREACH` : machine injoignable, `ENETUNREACH` : réseau
injoignable, `ETIMEDOUT`...) sont aussi classées « filtré ».

Remarque : sur 127.0.0.1 il n'y a normalement aucun pare-feu, donc aucun
port filtré. Pour la démonstration du compte rendu, une règle temporaire
`iptables -I INPUT -i lo -p tcp --dport 9997 -j DROP` a été ajoutée puis
retirée : le port 9997 apparaît alors « filtré » après 300 ms.

## 4. Pourquoi un `connect()` non bloquant ?

Avec un socket normal (bloquant), `connect()` vers un port filtré peut
bloquer le programme pendant **plus d'une minute** (le noyau renvoie
plusieurs fois le SYN avant d'abandonner). Scanner 1000 ports filtrés
prendrait des heures.

En mode non bloquant (`fcntl(fd, F_SETFL, O_NONBLOCK)`) :

1. `connect()` rend la main tout de suite avec `EINPROGRESS` ;
2. `select()` attend que le socket devienne « prêt en écriture », mais au
   plus `timeout_ms` millisecondes ;
3. si `select()` renvoie 0 : délai dépassé, port **filtré** ;
4. sinon `getsockopt(SO_ERROR)` donne le vrai résultat : 0 = **ouvert**,
   `ECONNREFUSED` = **fermé**, autre erreur = **filtré**.

C'est ce qui permet de choisir le délai maximum par port.

## 5. Ce que le scanner fait et ne fait pas

- Il fait une **connexion TCP complète** (« connect scan ») : la poignée de
  main va jusqu'au bout, la connexion est donc visible dans les journaux de
  la cible. Il ne fait pas de « scan SYN furtif » (qui demanderait des
  sockets bruts et les droits root).
- Il ne teste que **TCP** et **IPv4**.

> À utiliser uniquement sur des machines qui vous appartiennent ou avec
> autorisation écrite.
