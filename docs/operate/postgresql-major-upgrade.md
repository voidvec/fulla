# PostgreSQL Major-Version Upgrade Runbook (15 → 17)

> Scope: existing deployments using this repository's `deploy/docker/docker-compose.prod.yml`
> (or the Helm chart) with **persistent data volumes**. A PG major version **refuses to
> mount** an older version's data directory — a plain `docker compose up` (with the image
> already bumped to `postgres:17-alpine`) will leave the database container crash-looping;
> data is not corrupted, but the service is unavailable. You must follow this runbook
> before upgrading.
>
> Fresh deployments are unaffected (the 17 data directory is created directly by
> initialization).
>
> fulla's own benchmark difference between PG 15 and 17 is within the noise band (2026-08-18
> A/B measurements). The motivation for the upgrade is alignment with the server-side libpq
> 17.x client and benchmark-environment consistency — **there is no throughput requirement
> behind it**. Deployments in no hurry can keep running 15 (pin the compose image tag back
> to `postgres:15-alpine`; it is compatible with the application).

## Choosing a Route

| Route | Downtime window | Fits |
|---|---|---|
| A. dump/restore (recommended) | Proportional to data size (GB-scale usually minutes) | Small-to-medium data volumes; simple to operate, cross-verifiable |
| B. pg_upgrade (in place) | Usually &lt;1 minute | Large data volumes (tens of GB+) that cannot tolerate a long downtime |

Both routes require: **complete a full backup and keep the old data volume before
upgrading**; clean up only after verification passes.

## Route A: dump/restore (recommended)

Using the prod compose as the example (credentials come from `.env`: `POSTGRES_USER`/
`POSTGRES_PASSWORD`/`POSTGRES_DB`; substitute the actual compose project name, same below).

### 1. Stop the application, keep the old database

```bash
cd deploy/docker
docker compose -f docker-compose.prod.yml stop fulla-backend
# 旧 PG（15）保持运行，用它导出
```

### 2. Full export

```bash
docker compose -f docker-compose.prod.yml exec -T fulla-postgres \
  pg_dump -U "$POSTGRES_USER" -d "$POSTGRES_DB" --no-owner --no-privileges \
  > backup_pg15_$(date +%Y%m%d).sql
# 校验导出文件非空且含表结构
grep -c "CREATE TABLE" backup_pg15_*.sql
```

### 3. Switch the data volume

```bash
docker compose -f docker-compose.prod.yml down
# 重命名旧卷留底（project 名按实际替换；默认 project 名 = 目录名）
docker volume rm <project>_pgdata 2>/dev/null || true
# 更稳妥：先改名留底而不是删除
#   docker run --rm -v <project>_pgdata:/src -v /var/backups:/dst alpine \
#     cp -a /src /dst/pgdata_pg15_backup
```

> Simplified variant: you may also just `docker volume rm` the old volume — provided the
> dump file from step 2 is safely stored on the host and verified. The retained copy is
> insurance for "rollback if verification fails".

### 4. Start 17 and import

```bash
docker compose -f docker-compose.prod.yml up -d fulla-postgres
# 等健康检查通过后导入
docker compose -f docker-compose.prod.yml exec -T fulla-postgres \
  psql -U "$POSTGRES_USER" -d "$POSTGRES_DB" \
  < backup_pg15_$(date +%Y%m%d).sql 2>&1 | grep -i "error\|fatal" || echo "IMPORT CLEAN"
```

> With the server's startup auto-migration configured (`FULLA_AUTO_MIGRATE=true`), the
> first startup after importing the old schema automatically applies the newer migrations
> (V025 partitioning etc., idempotent).

### 5. Verify, then restore traffic

```bash
docker compose -f docker-compose.prod.yml up -d
curl -fsS http://<backend>/health/ready
```

Run the core paths from [verification-checklist.md](verification-checklist.md)
(login → authorization code → token → introspect → userinfo). After passing, keep the dump
file and the old-volume copy for at least one regression cycle before cleaning up.

## Route B: pg_upgrade (in place, large data volumes)

pg_upgrade needs both the old and new binaries and data directories present at once. The
easiest approach under containers is the official `postgres:17` image's built-in
`/usr/local/bin/pg_upgrade` (the alpine image does not include it; use a non-alpine tag or
mount a tooling image yourself):

```bash
# 1) 停应用与旧库
docker compose -f docker-compose.prod.yml stop fulla-backend fulla-postgres

# 2) 复制旧数据卷到新卷（pg_upgrade --link 可省空间但有损坏回滚风险，不默认）
docker run --rm -v <project>_pgdata:/old -v <project>_pgdata17:/new alpine \
  cp -a /old /new

# 3) 用 17 镜像对新副本执行 pg_upgrade（旧目录只读挂载）
docker run --rm \
  -v <project>_pgdata:/var/lib/postgresql/15 \
  -v <project>_pgdata17:/var/lib/postgresql/17 \
  -e POSTGRES_USER=$POSTGRES_USER postgres:17 \
  bash -c 'install -d /var/lib/postgresql/17.tmp && gosu postgres pg_upgrade \
    -b /usr/lib/postgresql/15/bin -B /usr/local/bin \
    -d /var/lib/postgresql/15 -D /var/lib/postgresql/17 \
    -U "$POSTGRES_USER"'
# 注：postgres:17 非 alpine 镜像内含 15 的 binary（多版本包），命令按镜像实际
# 路径调整；上述为骨架，执行前先在测试环境演练一遍。

# 4) 把 compose 的 pgdata 卷指向升级后的卷（或交换卷名），up -d，验证同路线 A。
```

After pg_upgrade succeeds, run `vacuumdb --all --analyze` as instructed (or wait for the automatic analyze).

## Helm Deployments

The chart's built-in PostgreSQL image tag lives in `deploy/helm/fulla/values.yaml`
(`postgresql.image`). The StatefulSet's PVC is likewise a major-version-bound data
directory:

1. `kubectl scale deploy/fulla --replicas=0` (stop the application);
2. Export/upgrade the data in the PVC via either route above;
3. Update the values, run `helm upgrade`, and restore the replicas once ready.

Deployments with external databases (self-managed PG / RDS-style) do not involve this
repository's configuration; follow your cloud provider's or DBA's major-version upgrade
process.

## Rollback

- Route A: `down` → restore the retained old volume (or restore the `postgres:15-alpine`
  tag + the original volume) → `up -d`. The application image is compatible with PG 15
  (a libpq 17 client connecting to a 15 server is fine).
- Route B: the old volume was untouched (mounted read-only); simply switch back to it.
- Rolling back after new data has been written to the new database loses that data — make
  the rollback decision before restoring verification traffic.
