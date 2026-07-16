# ScratchBird create-time bootstrap platform profile template.
#
# Copy this file into a private instance and replace every placeholder value
# before invoking SBsec bootstrap. The only accepted service identities are
# local scratchbird/scratchbird on POSIX and NT SERVICE\scratchbird on Windows.
#
# These fields identify only the operating-system account used for file and
# directory ownership and process execution. They neither name nor grant a
# ScratchBird database principal, role, authentication right, or security
# authority. Root/Administrator is the sole create-time OS authorization gate;
# after that gate, these fields control only ownership, privilege drop, ACL
# handoff, and process execution. SBsec creates the explicitly requested first
# database principal through the engine-owned MGA bootstrap transaction.
schema_id = scratchbird.bootstrap_platform_profile.v1
platform = operator_required
service_identity = operator_required
service_group = operator_required
