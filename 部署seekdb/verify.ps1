param(
    [string]$Container = 'oceanbase2026-seekdb'
)

$ErrorActionPreference = 'Stop'

$state = & docker inspect $Container --format '{{.State.Running}}'
if ($LASTEXITCODE -ne 0 -or $state -ne 'true') {
    throw "Container is not running: $Container"
}

$sql = & docker exec $Container mysql --host=127.0.0.1 --port=2881 --user=root --batch --skip-column-names --execute='SELECT 1 + 1;'
if ($LASTEXITCODE -ne 0 -or ($sql -join "`n").Trim() -ne '2') {
    throw 'SQL verification failed.'
}

$version = & docker exec $Container mysql --host=127.0.0.1 --port=2881 --user=root --batch --skip-column-names --execute='SELECT VERSION();'
if ($LASTEXITCODE -ne 0 -or $version -notmatch 'seekdb') {
    throw 'Server did not identify itself as seekdb.'
}

$raw = & docker exec $Container cat /var/lib/seekdb/run/telemetry.json
if ($LASTEXITCODE -ne 0) { throw 'Cannot read instance telemetry.' }
$record = ($raw -join "`n") | ConvertFrom-Json
$uuid = [Guid]::Empty
if (-not [Guid]::TryParseExact([string]$record.content.id, 'D', [ref]$uuid)) {
    throw 'Instance did not generate a valid UUID.'
}

$httpCode = & docker exec $Container curl --silent --show-error --output /dev/null --write-out '%{http_code}' --max-time 15 --head https://openwebapi.oceanbase.com/api/web/oceanbase/report
if ($LASTEXITCODE -ne 0 -or $httpCode -ne '200') {
    throw 'Telemetry endpoint connectivity check failed.'
}

[PSCustomObject]@{
    Container = $Container
    Version = ($version -join "`n").Trim()
    SqlCheck = 2
    DeploymentUUID = $uuid.ToString('D')
    TelemetryEndpointReachable = $true
    TelemetryDelivery = 'Not verified by this script; use official assessment.'
} | ConvertTo-Json
