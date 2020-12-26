import { Reader } from "ckb-js-toolkit";
import {
  since,
  utils,
  Cell,
  Hash,
  HashType,
  Indexer,
  HexNumber,
  HexString,
  Script,
} from "@ckb-lumos/base";
import { TransactionSkeletonType, addressToScript } from "@ckb-lumos/helpers";
import {
  PoAData,
  parsePoAData,
  parsePoASetup,
  serializePoAData,
} from "./config";

type State = "Yes" | "YesIfFull" | "No";

function pushAndFix(
  txSkeleton: TransactionSkeletonType,
  cell: Cell,
  field: "inputs" | "outputs"
) {
  const index = txSkeleton.get(field).count();
  txSkeleton = txSkeleton.update(field, (items) => items.push(cell));
  return txSkeleton.update("fixedEntries", (fixedEntries) => {
    return fixedEntries.push({
      field,
      index,
    });
  });
}

export class PoAGenerator {
  ckbAddress: string;
  indexer: Indexer;

  constructor(ckbAddress: string, indexer: Indexer) {
    this.ckbAddress = ckbAddress;
    this.indexer = indexer;
  }

  async shouldIssueNewBlock(
    medianTimeHex: HexNumber,
    tipCell: Cell
  ): Promise<State> {
    const { poaData, poaSetup, aggregatorIndex } = await this._queryPoAInfos(
      tipCell
    );
    const medianTime = BigInt(medianTimeHex);
    if (
      medianTime <
        poaData.round_initial_subtime + BigInt(poaSetup.subblock_intervals) &&
      poaData.subblock_index + 1 < poaSetup.subblocks_per_interval
    ) {
      return "YesIfFull";
    }
    const steps =
      (aggregatorIndex +
        poaSetup.identities.length -
        poaData.aggregator_index) %
      poaSetup.identities.length;
    if (
      medianTime >=
      poaData.round_initial_subtime +
        BigInt(poaSetup.subblock_intervals) * BigInt(steps)
    ) {
      return "Yes";
    }
    return "No";
  }

  async fixTransactionSkeleton(
    medianTimeHex: HexNumber,
    txSkeleton: TransactionSkeletonType
  ): Promise<TransactionSkeletonType> {
    const {
      poaData,
      poaDataCell,
      poaSetup,
      poaSetupCell,
      aggregatorIndex,
      script,
      scriptHash,
    } = await this._queryPoAInfos(txSkeleton.get("inputs").get(0)!);
    txSkeleton = txSkeleton.update("cellDeps", (cellDeps) =>
      cellDeps.push({
        out_point: poaSetupCell.out_point!,
        dep_type: "code",
      })
    );
    txSkeleton = pushAndFix(txSkeleton, poaDataCell, "inputs");
    const medianTime = BigInt(medianTimeHex);
    let newPoAData: PoAData;
    if (
      medianTime <
        poaData.round_initial_subtime + BigInt(poaSetup.subblock_intervals) &&
      poaData.subblock_index + 1 < poaSetup.subblocks_per_interval
    ) {
      // New block in current round
      newPoAData = {
        round_initial_subtime: poaData.round_initial_subtime,
        subblock_subtime: poaData.subblock_subtime + 1n,
        subblock_index: poaData.subblock_index + 1,
        aggregator_index: poaData.aggregator_index,
      };
    } else {
      // New block in new round
      newPoAData = {
        round_initial_subtime: medianTime,
        subblock_subtime: medianTime,
        subblock_index: 0,
        aggregator_index: aggregatorIndex,
      };
    }
    // Update PoA cell since time
    // TODO: block interval handling
    txSkeleton = txSkeleton.update("inputSinces", (inputSinces) => {
      return inputSinces.set(
        0,
        since.generateSince({
          relative: false,
          type: "blockTimestamp",
          value: newPoAData.subblock_subtime,
        })
      );
    });
    const newPackedPoAData = new Reader(
      serializePoAData(newPoAData)
    ).serializeJson();
    const newPoADataCell = {
      cell_output: poaDataCell.cell_output,
      data: newPackedPoAData,
    };
    txSkeleton = pushAndFix(txSkeleton, newPoADataCell, "outputs");
    // Add one owner cell if not exists already
    const ownerCells = txSkeleton.get("inputs").filter((cell) => {
      const currentScriptHash = utils.computeScriptHash(cell.cell_output.lock);
      return currentScriptHash === scriptHash;
    });
    if (ownerCells.count() === 0) {
      const ownerCell = await this._queryOwnerCell(script);
      txSkeleton = pushAndFix(txSkeleton, ownerCell, "inputs");
      txSkeleton = pushAndFix(txSkeleton, ownerCell, "outputs");
    }
    return txSkeleton;
  }

  async _queryPoAInfos(tipCell: Cell) {
    const poaDataCellTypeHash = new Reader(
      new Reader(tipCell.cell_output.lock.args).toArrayBuffer().slice(32)
    );
    if (poaDataCellTypeHash.length() !== 32) {
      throw new Error("Invalid PoA cell lock args!");
    }
    const poaDataCell = await this._queryPoaStateCell(
      poaDataCellTypeHash.serializeJson()
    );
    const poaData = parsePoAData(new Reader(poaDataCell.data).toArrayBuffer());
    const poaSetupCellTypeHash = new Reader(
      new Reader(tipCell.cell_output.lock.args).toArrayBuffer().slice(0, 32)
    );
    const poaSetupCell = await this._queryPoaStateCell(
      poaSetupCellTypeHash.serializeJson()
    );
    const poaSetup = parsePoASetup(
      new Reader(poaSetupCell.data).toArrayBuffer()
    );
    if (!poaSetup.interval_uses_seconds) {
      throw new Error("TODO: implement block interval PoA");
    }
    let script = addressToScript(this.ckbAddress);
    let scriptHash = utils.computeScriptHash(script);
    let truncatedScriptHash = new Reader(
      new Reader(scriptHash).toArrayBuffer().slice(0, poaSetup.identity_size)
    ).serializeJson();
    const aggregatorIndex = poaSetup.identities.findIndex(
      (identity) => identity === truncatedScriptHash
    );
    if (aggregatorIndex < 0) {
      throw new Error("Specified identity cannot be located!");
    }
    return {
      poaData,
      poaDataCell,
      poaSetup,
      poaSetupCell,
      aggregatorIndex,
      scriptHash,
      script,
    };
  }

  async _queryPoaStateCell(args: Hash) {
    const query = {
      lock: {
        code_hash:
          "0x00000000000000000000000000000000000000000000000000545950455f4944",
        hash_type: "type" as HashType,
        args: args,
      },
    };
    const collector = this.indexer.collector(query);
    const results = [];
    for await (const cell of collector.collect()) {
      results.push(cell);
    }
    if (results.length !== 1) {
      throw new Error(`Invalid number of poa state cells: ${results.length}`);
    }
    return results[0];
  }

  async _queryOwnerCell(script: Script) {
    const query = {
      lock: script,
    };
    const collector = this.indexer.collector(query);
    for await (const cell of collector.collect()) {
      return cell;
    }
    throw new Error("Cannot find any owner cell!");
  }
}