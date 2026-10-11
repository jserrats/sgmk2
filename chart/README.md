# sgmk2-cv chart

Runs the web controller from `cv/` (image `ghcr.io/jserrats/sgmk2/cv`, built by
`.github/workflows/cv-image.yml`). Not packaged or published: deploy it from this
directory. `.github/workflows/lint-chart.yml` lints and renders it whenever it changes.

## Values

| Value | Default | Meaning |
| --- | --- | --- |
| `image.repository` | `ghcr.io/jserrats/sgmk2/cv` | |
| `image.tag` | `""` (→ `latest`) | pin `sha-<commit>`; ArgoCD never notices `latest` moving |
| `image.pullPolicy` | `IfNotPresent` | |
| `imagePullSecrets` | `[]` | only if the GHCR package is made private again |
| `mqtt.broker` / `mqtt.port` | `mqtt.jserrats.xyz` / `1883` | |
| `mqtt.clientId` | `sgmk2-controller` | fixed per controller; see "One replica" below |
| `camera.url` | `http://sgmk2-cam.iot:8080/` | the ESPHome camera's MJPEG stream |
| `service.type` / `service.port` | `ClusterIP` / `5000` | |
| `ingress.*` | disabled | the UI has no auth and can fire the turret |
| `hostAliases`, `dnsPolicy`, `dnsConfig`, `hostNetwork` | unset | fallbacks if the camera name doesn't resolve |
| `probes.enabled` | `true` | TCP checks on the http port |
| `resources` | 50m / 128Mi requests, 512Mi limit | |
| `terminationGracePeriodSeconds` | `10` | the app stops the turret and exits on SIGTERM |
| `podSecurityContext`, `securityContext` | non-root uid 1000, RuntimeDefault seccomp, no privilege escalation, all caps dropped | |
| `podAnnotations`, `podLabels`, `nodeSelector`, `tolerations`, `affinity` | empty | |

The pod doesn't mount a service account token.

### One replica

The deployment is fixed at **one replica with the `Recreate` strategy**, and that's not
configurable. The app connects to MQTT with a fixed client id, so two pods would keep
kicking each other off the broker, and all control state is in-process anyway. The cost
is a few seconds of no control during an upgrade.

### Camera DNS

`sgmk2-cam.iot` is a LAN name. On ganymede, CoreDNS forwards to the node's resolver,
which knows `.iot`, so it should resolve with nothing set. Elsewhere, map it:

```yaml
hostAliases:
  - ip: 192.168.1.50   # the camera's LAN address
    hostnames:
      - sgmk2-cam.iot
```

or set `camera.url` to an IP, or use `dnsPolicy`/`dnsConfig` or `hostNetwork: true`.

### Reaching the UI

Ingress is off by default. Use a port-forward and open http://127.0.0.1:5000/:

```
kubectl -n sgmk2 port-forward svc/sgmk2-sgmk2-cv 5000
```

(The service is named `<release>-sgmk2-cv`, or just the release name if it contains
`sgmk2-cv`.) If you do enable `ingress` behind ingress-nginx, raise `proxy-read-timeout`
and turn off `proxy-buffering` (see `values.yaml`) or the socket.io connection and the
MJPEG stream get cut off.

## ArgoCD

An Application for the `cloud` repo, as `argocd/apps/sgmk2.yaml`. Pin both
`targetRevision` and `image.tag` to the same commit:

```yaml
# The SGMK2 web controller, from the chart in the turret's own repo
# (github.com/jserrats/sgmk2, chart/).
#
# Two sources: the chart, and this repo as `$values`, for cluster.yaml.
apiVersion: argoproj.io/v1alpha1
kind: Application
metadata:
  name: sgmk2
  namespace: argocd
  finalizers:
    - resources-finalizer.argocd.argoproj.io
spec:
  project: default
  sources:
    # Public, so ArgoCD needs no deploy key for it.
    - repoURL: https://github.com/jserrats/sgmk2.git
      path: chart
      # Pinned to a commit, like every other version in this repo.
      targetRevision: <commit>
      helm:
        valueFiles: [$values/argocd/cluster.yaml]
        valuesObject:
          image:
            tag: sha-<short commit>
    - repoURL: git@github.com:jserrats/cloud.git
      targetRevision: main
      ref: values
  destination:
    server: https://kubernetes.default.svc
    namespace: sgmk2
  syncPolicy:
    automated: { prune: true, selfHeal: true }
    syncOptions: [CreateNamespace=true]
```

`docker/metadata-action`'s `type=sha` tag is the 7-character short sha
(`sha-1a2b3c4`). The chart doesn't read anything from `cluster.yaml` yet; it's loaded
so that a future ingress can use `domain` the way the other apps do.

To install without ArgoCD:

```
helm upgrade --install sgmk2 ./chart -n sgmk2 --create-namespace \
  --set image.tag=sha-<commit>
```
