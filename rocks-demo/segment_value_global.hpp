// Canonical segment value layout: one V2 value per segment_uid (K ≈ N, no
// ghost cross-bucket replication). The master/ghost materialization subsystem
// (MaterializedSegmentValue / DecodeAndMaterializeSegmentValue / V3 value
// encoding / E7 key type_flags) has been removed. Read paths decode the V2
// value directly via segment_value_codec.hpp.

#pragma once

#include "segment_value_codec.hpp"
