
/*
 * DOMAIN(name, num, index_kind, is_physical)
 *
 * @name: an all caps token, suitable for use in generating an enum member and
 *        appending to an event name in sysfs.
 * @num: the number corresponding to the domain as given in documentation. We
 *       assume the catalog domain and the hcall domain have the same numbering
 *       (so far they do), but this may need to be changed in the future.
 * @index_kind: a stringifiable token describing the meaning of the index within the
 *              given domain. Must fit the parsing rules of the perf sysfs api.
 * @is_physical: true if the domain is physical, false otherwise (if virtual).
 */
DOMAIN(PHYSICAL_CHIP, 0x01, chip, true)
DOMAIN(PHYSICAL_CORE, 0x02, core, true)
DOMAIN(VIRTUAL_PROCESSOR_HOME_CORE, 0x03, vcpu, false)
DOMAIN(VIRTUAL_PROCESSOR_HOME_CHIP, 0x04, vcpu, false)
DOMAIN(VIRTUAL_PROCESSOR_HOME_NODE, 0x05, vcpu, false)
DOMAIN(VIRTUAL_PROCESSOR_REMOTE_NODE, 0x06, vcpu, false)
