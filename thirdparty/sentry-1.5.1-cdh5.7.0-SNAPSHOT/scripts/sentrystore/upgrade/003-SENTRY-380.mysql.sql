-- SENTRY-380
ALTER TABLE `SENTRY_DB_PRIVILEGE` DROP `GRANTOR_PRINCIPAL`;
ALTER TABLE `SENTRY_ROLE` DROP `GRANTOR_PRINCIPAL`;
ALTER TABLE `SENTRY_GROUP` DROP `GRANTOR_PRINCIPAL`;

ALTER TABLE `SENTRY_ROLE_DB_PRIVILEGE_MAP` ADD `GRANTOR_PRINCIPAL` VARCHAR(128) CHARACTER SET utf8 COLLATE utf8_bin;
ALTER TABLE `SENTRY_ROLE_GROUP_MAP` ADD `GRANTOR_PRINCIPAL` VARCHAR(128) CHARACTER SET utf8 COLLATE utf8_bin;