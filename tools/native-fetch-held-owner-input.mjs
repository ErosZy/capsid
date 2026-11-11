import {heldCodecBranchBasePlans as allPlans,heldCodecBranchReply,heldCodecBranchInputPaths as originalPaths} from './native-fetch-held-branch-input.mjs';
export const heldCodecBranchBasePlans=Object.freeze(allPlans.filter(plan=>['codec-edge-crc-8197','codec-edge-member-second-crc'].includes(plan.id)));
export {heldCodecBranchReply};
export const heldCodecBranchInputPaths=Object.freeze({...originalPaths,ownerSourceModel:'tools/native-fetch-held-owner-peer.mjs',ownerPlanModel:'tools/native-fetch-held-owner-input.mjs',branchFixture:'tests/fixtures/native-fetch-error-owner.mjs'});
