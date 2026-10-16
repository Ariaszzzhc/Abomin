import { a } from './cycle-a.ts';
export const b = 22;
export function first() { return a; }
