# webframework — framework web MVC en MATL-ABC

Serveur HTTP réel écrit en MATL-ABC : routes déclaratives en arbre,
un thread détaché par connexion, base SQLite, vues HTML lues par le
FileWrapper, et handlers = lambdas `funcptr<(str)str>`.

Voir `matlabc_doc/40_webframework.md` pour la documentation complète,
y compris la section « Monomorphisation sans virtualisation ».

## Prérequis

- `libsqlite3` (`libsqlite3.so.0`) et `libpthread` (standard).
- Les bibliothèques `file_wrapper` (posixshim) doivent être présentes
  au chemin indiqué dans `conf.toml`.

## Build

Le `conf.toml` utilise des **hooks de build** (voir
`matlabc_doc/41_build_hooks.md`) : le compilateur compile lui-même le
shim C et rend le posixshim disponible. **Une seule commande** :

```bash
matlabc main.mabc --conf conf.toml
```

Le pipeline affiche alors ses étapes de préparation :
```
  [hooks:pre] 2 etape(s)
    (1/2) compiler le shim web (threads + sqlite3)   ✔ OK
    (2/2) rendre le posixshim disponible localement  ✔ OK
```

(Si tu préfères compiler le shim à la main, la commande reste :
`gcc -shared -fPIC webshim.c -o libwebshim.so -l:libsqlite3.so.0 -lpthread`.)

## Lancer et tester

Le serveur tourne en mode démo (`server.max_requests = 6` dans
`main.mabc` : il s'arrête proprement après 6 requêtes ; retirer cette
ligne pour un serveur permanent).

```bash
LD_LIBRARY_PATH=. /tmp/webframework_out &    # ou ./webframework_out selon output

curl http://localhost:8080/                       # vue home (fichier)
curl http://localhost:8080/api/v1/sante           # nb de messages (SQLite)
curl http://localhost:8080/api/v1/messages        # liste HTML (SQLite)
curl http://localhost:8080/app/apropos            # vue à-propos (fichier)
curl http://localhost:8080/inexistant             # 404
```

Grâce aux hooks, `libwebshim.so` et `libposixshim.so` sont dans le
dossier courant : `LD_LIBRARY_PATH=.` suffit.

## Structure

```
main.mabc              tout le framework (Vec, Model, routes, serveur)
webshim.c              pont C : threads + sqlite3
headers/webshim.mabh   interface du pont
headers/*.mabh         extra_file / posixshim (FileWrapper)
views/*.html           les vues (V du MVC)
conf.toml              lie les 3 bibliothèques
```

## Architecture MVC

- **Model** : `MessageModel` → table SQLite `messages`.
- **View** : `views/*.html` lus par `TextFile` (FileWrapper) via
  `render_view(path)`.
- **Controller** : un handler-lambda `funcptr<(str)str>` par route,
  passé inline à `new GET(...)` / `new POST(...)`.

Le routeur est un arbre `NodeGroup` déclaratif, aplati récursivement
en une liste de routes avec chemins complets avant le démarrage.

## Déploiement (DevOps)

Le `conf.toml` inclut une section `[deploy.*]`. Pour générer le
Dockerfile, les manifests Kubernetes et le pipeline CI (voir
`matlabc_doc/42_devops_deploy.md`) :

```bash
matlabc deploy --conf conf.toml
```

Produit un Dockerfile multi-stage non-root, `k8s/deployment.yaml` +
`k8s/service.yaml` (2 replicas, LoadBalancer, namespace `recurstream`)
et `.github/workflows/matlabc.yml`. Relis-les avant de déployer.
