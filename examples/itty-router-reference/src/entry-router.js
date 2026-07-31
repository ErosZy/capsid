import { createApplication } from './shared-handlers.js';

const application = createApplication('router');

export { application };
export default { fetch: application.fetch };
