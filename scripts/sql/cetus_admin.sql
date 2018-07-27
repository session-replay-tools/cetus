SET NAMES utf8mb4;
SET FOREIGN_KEY_CHECKS = 0;

-- ----------------------------
-- Table structure for objects
-- ----------------------------
DROP TABLE IF EXISTS `objects`;
CREATE TABLE `objects` (
  `object_name` varchar(64) NOT NULL,
  `object_value` text NOT NULL,
  `mtime` timestamp NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
  PRIMARY KEY (`object_name`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8;


-- ----------------------------
-- Table structure for services
-- ----------------------------
DROP TABLE IF EXISTS `services`;
CREATE TABLE `services` (
  `id` varchar(64) NOT NULL,
  `data` varchar(64) NOT NULL,
  `start_time` timestamp NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
  PRIMARY KEY (`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8;

-- ----------------------------
-- Records of services
-- ----------------------------
BEGIN;
INSERT INTO `services` VALUES ('0.0.0.0:3306', 'proxy');
INSERT INTO `services` VALUES ('0.0.0.0:3307', 'admin');
COMMIT;

-- ----------------------------
-- Table structure for settings
-- ----------------------------
DROP TABLE IF EXISTS `settings`;
CREATE TABLE `settings` (
  `option_key` varchar(64) NOT NULL,
  `option_value` varchar(1024) NOT NULL,
  PRIMARY KEY (`option_key`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8;

-- ----------------------------
-- Records of settings
-- ----------------------------
BEGIN;
INSERT INTO `settings` VALUES ('admin-address', '0.0.0.0:3307');
INSERT INTO `settings` VALUES ('admin-password', '');
INSERT INTO `settings` VALUES ('admin-username', 'admin');
INSERT INTO `settings` VALUES ('check-slave-delay', 'true');
INSERT INTO `settings` VALUES ('daemon', 'false');
INSERT INTO `settings` VALUES ('default-db', '');
INSERT INTO `settings` VALUES ('default-pool-size', '100');
INSERT INTO `settings` VALUES ('default-username', '');
INSERT INTO `settings` VALUES ('disable-dns-cache', 'true');
INSERT INTO `settings` VALUES ('disable-threads', 'false');
INSERT INTO `settings` VALUES ('keepalive', 'true');
INSERT INTO `settings` VALUES ('log-backtrace-on-crash', 'true');
INSERT INTO `settings` VALUES ('log-file', '/usr/local/cetus/logs/cetus.log');
INSERT INTO `settings` VALUES ('log-level', 'info');
INSERT INTO `settings` VALUES ('long-query-time', '100');
INSERT INTO `settings` VALUES ('max-alive-time', '600');
INSERT INTO `settings` VALUES ('max-open-files', '65536');
INSERT INTO `settings` VALUES ('max-resp-size', '10485760');
INSERT INTO `settings` VALUES ('pid-file', 'cetus.pid');
INSERT INTO `settings` VALUES ('plugin-dir', '/usr/local/cetus/lib/cetus/plugins');
INSERT INTO `settings` VALUES ('plugins', 'proxy,admin');
INSERT INTO `settings` VALUES ('proxy-address', '0.0.0.0:3306');
INSERT INTO `settings` VALUES ('proxy-backend-addresses', '');
INSERT INTO `settings` VALUES ('proxy-read-only-backend-addresses', '');
INSERT INTO `settings` VALUES ('read-master-percentage', '0');
INSERT INTO `settings` VALUES ('slave-delay-down', '5');
INSERT INTO `settings` VALUES ('slave-delay-recover', '1');
INSERT INTO `settings` VALUES ('verbose-shutdown', 'true');
COMMIT;

SET FOREIGN_KEY_CHECKS = 1;
