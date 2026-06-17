-- 00_create_database.sql
-- Optional. Executed by run_example_database.sh only when SB_CREATE_DB=1.
--
-- CREATE DATABASE is an engine lifecycle operation. Adjust the database name
-- (and any create options your build supports) to match your installation, and
-- note that some deployments require creating the database through an
-- administrative/bootstrap connection rather than the target database itself.
-- If the database already exists, leave SB_CREATE_DB=0 and skip this script.

CREATE DATABASE example_db;
