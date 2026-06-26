package org.rocksdb;

/**
 * EXPERIMENTAL (ST fork): configures {@link ReadOptions#experimental_st_prune_scan}.
 * Used by TMan-RocksDB ST arm with TMan RowKey + TMRV1/STPT values.
 */
@Experimental("ST fork spatio-temporal prune scan")
public final class ExperimentalStPruneScan {
  public boolean enable = false;
  public boolean fileLevelEnable = true;
  public boolean blockLevelEnable = true;
  public boolean keyLevelEnable = true;
  public boolean valueLevelPointTruthEnable = true;
  public boolean fileLevelTimeBucketRtreeEnable = false;
  public boolean keyLevelAdaptiveGateEnable = false;
  public boolean blockLevelAdaptiveGateEnable = false;
  public boolean keyLevelBoundaryBlocksOnlyEnable = false;
  public float keyLevelDisableOverlapThreshold = 0.6f;
  public float blockLevelDisableOverlapThreshold = 0.85f;
  public int tMinSec = 0;
  public int tMaxSec = 0;
  public float xMin = 0.f;
  public float yMin = 0.f;
  public float xMax = 0.f;
  public float yMax = 0.f;

  public ExperimentalStPruneScan setWindow(
      int tMinSec, int tMaxSec, float xMin, float yMin, float xMax, float yMax) {
    this.tMinSec = tMinSec;
    this.tMaxSec = tMaxSec;
    this.xMin = xMin;
    this.yMin = yMin;
    this.xMax = xMax;
    this.yMax = yMax;
    return this;
  }
}
