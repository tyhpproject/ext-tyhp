<?php

/* Same sequence as Tyhp\Io\Dns\DnsLookup::run. An empty list means unresolved. */
function tyhp_dns_oracle(string $host): array
{
    if (\filter_var($host, \FILTER_VALIDATE_IP) !== false) {
        return [$host];
    }
    $addresses = [];
    $ipv4 = \gethostbynamel($host);
    if (\is_array($ipv4)) {
        foreach ($ipv4 as $address) {
            if (\is_string($address)) {
                $addresses[] = $address;
            }
        }
    }
    $records = @\dns_get_record($host, \DNS_AAAA);
    if (\is_array($records)) {
        foreach ($records as $record) {
            $ipv6 = \is_array($record) ? ($record['ipv6'] ?? null) : null;
            if (\is_string($ipv6)) {
                $addresses[] = $ipv6;
            }
        }
    }
    if (\count($addresses) === 0) {
        return [];
    }
    return \array_values(\array_unique($addresses));
}

/* dnsStart, wait on the pipe, dnsTake. */
function tyhp_dns_run(string $host): array
{
    $started = \Tyhp\Ext::dnsStart($host);
    tyhp_dns_wait($started['pipe']);
    return \Tyhp\Ext::dnsTake($started['id']);
}

function tyhp_dns_wait($pipe, int $seconds = 30): void
{
    $read = [$pipe];
    $write = null;
    $except = null;
    $ready = \stream_select($read, $write, $except, $seconds);
    if ($ready !== 1) {
        throw new \RuntimeException('stream_select did not see the DNS pipe, result ' . \var_export($ready, true));
    }
}
