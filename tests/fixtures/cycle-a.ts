import { b } from './cycle-b.ts';
export const a = 20;
export function sum() { return a + b; }
