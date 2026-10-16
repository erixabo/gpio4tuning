# GPIO4Tuning

A `gpio4tuningd` egy Raspberry Pi GPIO4 kimenetét vezérlő, TCP-parancsokat
fogadó segédprogram. A Pi-n telepített `librpitx` segítségével egyszerű,
folyamatos vivőt indít, állít le és hangol. Nem generál CW- vagy SSTV-jelet;
ilyen adóprogramok mellett a frekvenciakorrekció kiszámításának és
alkalmazásának mintáját adja.

## Miért kell frekvenciakorrekció?

A GPIO4-en előállított RF frekvencia eltérhet a programnak megadott értéktől.
Az eltérés függhet a Pi példányától, az órajeltől, a hőmérséklettől és a
körülményektől. A kért frekvencia tehát önmagában nem bizonyítja, hogy a
kimenő vivő ténylegesen ott van.

A korrekcióhoz szükség van egy **megfigyelő vevőre**, amelynek frekvenciája
ismert és kellően stabil. A vevő lehet ugyanazon a Pi-n vagy egy másik gépen.
Például egy SDR-vevő és annak spektruma vagy demodulált hangja használható az
eltérés megállapítására. A vevő nem vezérli a GPIO-t: az adó és a vevő
frekvenciaadatait kell összevetni, majd az előjeles korrekciót beállítani.

A daemon nem mér frekvenciát és nem kalibrál automatikusan. A kezelő a
megfigyelő vevő alapján állapítja meg a korrekciót. Ez a mérés egy adott Pi
adóláncára és adott körülményekre vonatkozik; hőmérséklet-változás vagy más
hardver esetén újramérésre lehet szükség.

## Az `F`, `R`, `c`, `TC` és `T` viszonya

- `F` az **alapfrekvenciát** állítja be Hz-ben.
- `R` eltárolja a megadott megfigyelő-vevő célfrekvenciát, és kiszámítja a
  korrekciót: `c = F − R`.
- `TC` folyamatos vivőt indít az `F + c` frekvencián.
- `f` mindig az aktuálisan beállított vivőfrekvenciát mutatja: `TC` után a
  korrigált értéket, `T 0` után pedig az alapfrekvenciát.
- `T 1` nyers, korrekció nélküli vivőt indít; `T 0` leállítja a vivőt és
  visszaáll az alapfrekvenciára.

Példa: ha `F = 433100000`, és az `R` parancsnak `433099000` értéket adunk,
a daemon `c = +1000` Hz korrekciót ment. A `TC` ezért `433101000` Hz-en
indít vivőt. Az `f` ekkor `433101000` értéket ad; `T 0` után `433100000`-et.

```text
F 433100000
R 433099000
TXCORR 1000
TXFREQ 433100000
RPRT 0
TC
433101000
RPRT 0
f
433101000
RPRT 0
T 0
RPRT 0
f
433100000
RPRT 0
```

Ez a számítás és parancsviselkedés a daemon kezelőfelülete. A frekvenciát
külön vevővel kell megfigyelni; az `f` a parancsolt frekvenciát, nem a
műszerrel visszamért RF frekvenciát jelenti.

### Korrekció más adóprogramokban

Ugyanez az elv használható olyan GPIO4-es adóprogramoknál is, amelyeknek
átadható a vivő vagy középfrekvencia. Ha egy program a kívánt adási
frekvenciát `F_cél` változóval kezeli, a korrigált parancsfrekvencia alakja:

```text
F_parancs = F_cél + c
```

Például a PiSSTV, Hellook vagy CWook indításakor a programnak átadott
frekvenciát lehet ezzel a képlettel előállítani. Ezt az adott program
indítószkriptjébe vagy forrásába kell beépíteni; a `gpio4tuningd` önmagában
nem módosítja és nem indítja el ezeket a programokat. A korrekciót csak
**egyszer** szabad alkalmazni. Ha egy adóprogram vagy indítószkript már
korrigál, ne korrigálja újra egy másik réteg.

A folytonos vivő hasznos lehet hangolásra és megfigyelésre, de nem helyettesíti
a modulált adás külön ellenőrzését.

## Telepítés Raspberry Pi-re

A program fordítási függősége a **F5OEO `librpitx`** könyvtára:
<https://github.com/F5OEO/librpitx>. Ebből használja a
`librpitx/ngfmdmasync.h` fejlécet és a `librpitx` könyvtárat. A gpio4tuning
nem tartalmazza és nem tölti le automatikusan ezt a külön projektet.

A `make` és a `make install` előzetesen ellenőrzi, hogy a fejléc és a
linkelhető könyvtár megvan-e. Alapértelmezett telepítési helyük:
`/usr/local/include/librpitx/ngfmdmasync.h` és
`/usr/local/lib/librpitx.a` (vagy `librpitx.so`). Más prefix a
`LIBRPITX_PREFIX` változóval adható meg.

Telepítés Raspberry Pi OS-en, a hivatalos `librpitx` buildleírását követve:

```sh
sudo apt install git build-essential
git clone https://github.com/F5OEO/librpitx.git
cd librpitx/src
make
sudo make install
```

Ezután a gpio4tuning repó könyvtárában:

```sh
make
sudo make install
```

Ha a check hiányzó fejlécet vagy könyvtárat jelez, a `librpitx` fenti
fordítását és telepítését kell befejezni. Egyedi prefix esetén mindkét
parancsnak add át ugyanazt, például: `make LIBRPITX_PREFIX=/opt/librpitx`.

A telepítés a `gpio4tuningd.service` systemd egységet telepíti, engedélyezi és
elindítja. Alapértelmezés szerint a TCP szerver csak a helyi Pi-n
(`127.0.0.1:37357`) figyel, az adó pedig kikapcsolva marad.

### Távoli TCP-elérés beállítása

A parancsprotokoll nem hitelesít, ezért a szervert ne tedd ki nyilvános
hálózatra. Távoli gépről való használathoz a systemd egységben a `--bind`
értékét állítsd a Pi védett, elérhető LAN- vagy VPN-interfészének címére.
Például:

```sh
sudo systemctl edit gpio4tuningd
```

A megnyíló override-ba:

```ini
[Service]
ExecStart=
ExecStart=/usr/local/bin/gpio4tuningd --bind 192.168.1.20 --port 37357
```

A `192.168.1.20` helyére a saját Pi-címed kerüljön. Ezután:

```sh
sudo systemctl daemon-reload
sudo systemctl restart gpio4tuningd
```

Csatlakozás egy másik gépről:

```sh
nc <pi-címe> 37357
?
f
q
```

## TCP-parancsok

Egy sor egy ASCII parancs; LF, CR vagy CRLF lezárás elfogadott. A válaszok
`RPRT 0` sikeres, `RPRT 1 ...` hibás műveletet jeleznek.

| Parancs | Művelet |
|---|---|
| `?`, `help`, `h` | Beépített súgó |
| `f` | Aktuálisan beállított vivőfrekvencia lekérdezése Hz-ben |
| `F <Hz>` | Alapfrekvencia beállítása Hz-ben |
| `F+ <+Hz\|-Hz>` | Alapfrekvencia relatív léptetése |
| `r` | Utoljára megadott megfigyelő-vevő célfrekvencia lekérdezése |
| `R <Hz>` | Vevő-célfrekvencia rögzítése, `c = F − R` kiszámítása és mentése |
| `c` | Mentett korrekció lekérdezése Hz-ben |
| `c=<signed Hz>` | Korrekció közvetlen felülírása |
| `t` | A daemon által indított vivő állapota (`0` vagy `1`) |
| `T 1 [Hz]` | Nyers vivő indítása; opcionális Hz frekvenciával |
| `TC` | Folyamatos vivő indítása az `F + c` frekvencián |
| `T 0` | Saját vivő leállítása, GPIO4 bemenetre állítása, `f` visszaállítása az alapfrekvenciára |
| `free` | Saját vivő leállítása és GPIO4 bemenetre kényszerítése |
| `g` | GPIO4 foglaltsági állapot lekérdezése |
| `q` | TCP-kapcsolat lezárása |

A frekvenciatartomány 20 kHz–1,5 GHz. A korrekció a `/home/.tuner/txcorr`
fájlba kerül; más hely a `--txcorr` kapcsolóval adható meg.

A `g` ismert adófolyamatokat, a daemon saját kizáró zárját és a GPIO4
pinfunkcióját vizsgálja. Nem képes minden tetszőleges, együttműködés nélküli
program GPIO-használatát felismerni, ezért a `FREE` csak az ellenőrzött
állapotra vonatkozik. A `free` erőszakosan bemenetre állítja a GPIO4-et; ez
megszakíthat más, GPIO4-et használó programot is.

## Licenc és függőség

A gpio4tuning saját forrása **GNU GPL-3.0-only** licencű; a teljes licenc a
`LICENSE` fájlban található. A [F5OEO `librpitx` forrásrepo](https://github.com/F5OEO/librpitx)
szintén GPL-3.0 licencet jelöl; a függőség külön projekt marad, saját
forrásával és licencfájljával.
