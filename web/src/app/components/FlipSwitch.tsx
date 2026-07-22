import React, { forwardRef } from 'react';
import { Switch, Typography } from 'antd';
import type { SwitchProps } from 'antd';

const { Text } = Typography;

export interface FlipSwitchProps extends SwitchProps {
  label?: React.ReactNode;
}

const FlipSwitch = forwardRef<HTMLButtonElement, FlipSwitchProps>(({ label, ...props }, ref) => {
  const generatedId = React.useId();
  const id = props.id || generatedId;

  const handleLabelClick = (e: React.MouseEvent) => {
    e.preventDefault();
    if (!props.disabled && !props.loading && props.onChange) {
      props.onChange(!props.checked, e as unknown as React.MouseEvent<HTMLButtonElement>);
    }
  };

  const innerSwitch = (
    <Switch
      {...props}
      id={id}
      ref={ref}
      onClick={(checked, e) => {
        e.stopPropagation();
        if (props.onClick) {
          props.onClick(checked, e);
        }
      }}
    />
  );

  if (!label) {
    return innerSwitch;
  }

  return (
    <label
      htmlFor={id}
      onClick={handleLabelClick}
      style={{
        cursor: (props.disabled || props.loading) ? 'not-allowed' : 'pointer',
        display: 'inline-flex',
        alignItems: 'center',
        gap: 8,
      }}
    >
      <Text disabled={props.disabled || props.loading}>{label}</Text>
      {innerSwitch}
    </label>
  );
});

FlipSwitch.displayName = 'FlipSwitch';

export default FlipSwitch;
