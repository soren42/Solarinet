-- 008_probe_apptype.sql - additive probe application check fields.
--
-- Application-level probing is optional and best-effort at runtime. These
-- nullable/additive columns preserve existing TCP/UDP/ICMP target behavior while
-- allowing newer monitors to record the check type and an optional argument.

ALTER TABLE probeTarget
  ADD COLUMN probeType VARCHAR(16) NOT NULL DEFAULT 'tcp',
  ADD COLUMN checkArg VARCHAR(255) NULL;
